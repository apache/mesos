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

#include <stout/check.hpp>
#include <stout/gtest.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>

#include <stout/os/exists.hpp>
#include <stout/os/getcwd.hpp>
#include <stout/os/write.hpp>

#include <stout/tests/utils.hpp>

#include "uri/fetcher.hpp"

#include "uri/schemes/hdfs.hpp"
#include "uri/schemes/http.hpp"

namespace http = process::http;

using std::string;

using process::Future;
using process::Owned;
using process::Process;

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

  Try<Owned<uri::Fetcher>> fetcher = uri::fetcher::create();
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

  Try<Owned<uri::Fetcher>> fetcher = uri::fetcher::create();
  ASSERT_SOME(fetcher);

  AWAIT_FAILED(fetcher.get()->fetch(uri, os::getcwd()));
}


class HadoopFetcherPluginTest : public TemporaryDirectoryTest
{
public:
  virtual void SetUp()
  {
    TemporaryDirectoryTest::SetUp();

    // Create a fake hadoop command line tool. It emulates the hadoop
    // client's logic while operating on the local filesystem.
    hadoop = path::join(os::getcwd(), "hadoop");

    // The script emulating 'hadoop fs -copyToLocal <from> <to>'.
    // NOTE: We emulate a version call here which is exercised when
    // creating the HDFS client. But, we don't expect any other
    // command to be called.
    ASSERT_SOME(os::write(
        hadoop,
        "#!/bin/sh\n"
        "if [ \"$1\" = \"version\" ]; then\n"
        "  exit 0\n"
        "fi\n"
        "if [ \"$1\" != \"fs\" ]; then\n"
        "  exit 1\n"
        "fi\n"
        "if [ \"$2\" != \"-copyToLocal\" ]; then\n"
        "  exit 1\n"
        "fi\n"
        "cp $3 $4\n"));

    // Make sure the script has execution permission.
    ASSERT_SOME(os::chmod(
        hadoop,
        S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH));
  }

protected:
  string hadoop;
};


TEST_F(HadoopFetcherPluginTest, FetchExistingFile)
{
  string file = path::join(os::getcwd(), "file");

  ASSERT_SOME(os::write(file, "abc"));

  URI uri = uri::hdfs(file);

  uri::fetcher::Flags flags;
  flags.hadoop = hadoop;

  Try<Owned<uri::Fetcher>> fetcher = uri::fetcher::create(flags);
  ASSERT_SOME(fetcher);

  string dir = path::join(os::getcwd(), "dir");

  AWAIT_READY(fetcher.get()->fetch(uri, dir));

  EXPECT_SOME_EQ("abc", os::read(path::join(dir, "file")));
}


TEST_F(HadoopFetcherPluginTest, FetchNonExistingFile)
{
  URI uri = uri::hdfs(path::join(os::getcwd(), "non-exist"));

  uri::fetcher::Flags flags;
  flags.hadoop = hadoop;

  Try<Owned<uri::Fetcher>> fetcher = uri::fetcher::create(flags);
  ASSERT_SOME(fetcher);

  string dir = path::join(os::getcwd(), "dir");

  AWAIT_FAILED(fetcher.get()->fetch(uri, dir));
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
