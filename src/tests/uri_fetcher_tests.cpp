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
#include <stout/duration.hpp>
#include <stout/gtest.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>

#include <stout/os/exists.hpp>
#include <stout/os/getcwd.hpp>
#include <stout/os/ls.hpp>
#include <stout/os/write.hpp>

#include <stout/tests/utils.hpp>

#include <mesos/docker/spec.hpp>

#include "uri/fetcher.hpp"

#include "uri/schemes/docker.hpp"
#include "uri/schemes/file.hpp"
#include "uri/schemes/hdfs.hpp"
#include "uri/schemes/http.hpp"

namespace http = process::http;

using std::list;
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


TEST_F_TEMP_DISABLED_ON_WINDOWS(CurlFetcherPluginTest, CURL_ValidUri)
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


TEST_F_TEMP_DISABLED_ON_WINDOWS(CurlFetcherPluginTest, CURL_InvalidUri)
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


// This test verifies invoking 'fetch' by plugin name.
TEST_F_TEMP_DISABLED_ON_WINDOWS(CurlFetcherPluginTest, CURL_InvokeFetchByName)
{
  URI uri = uri::http(
      stringify(server.self().address.ip),
      "/TestHttpServer/test",
      server.self().address.port);

  EXPECT_CALL(server, test(_))
    .WillOnce(Return(http::OK("test")));

  Try<Owned<uri::Fetcher>> fetcher = uri::fetcher::create();
  ASSERT_SOME(fetcher);

  AWAIT_READY(fetcher.get()->fetch(uri, os::getcwd(), "curl"));

  EXPECT_TRUE(os::exists(path::join(os::getcwd(), "test")));
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

    // Make sure the script has execution permission. On Windows, we always
    // have permission.
#ifndef __WINDOWS__
    ASSERT_SOME(os::chmod(
        hadoop,
        S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH));
#endif // __WINDOWS__
  }

protected:
  string hadoop;
};


// TODO(hausdorff): Will not compile until HDFS is supported on Windows. See
// MESOS-5460.
#ifndef __WINDOWS__
TEST_F(HadoopFetcherPluginTest, FetchExistingFile)
{
  string file = path::join(os::getcwd(), "file");

  ASSERT_SOME(os::write(file, "abc"));

  URI uri = uri::hdfs(file);

  uri::fetcher::Flags flags;
  flags.hadoop_client = hadoop;

  Try<Owned<uri::Fetcher>> fetcher = uri::fetcher::create(flags);
  ASSERT_SOME(fetcher);

  string dir = path::join(os::getcwd(), "dir");

  AWAIT_READY(fetcher.get()->fetch(uri, dir));

  EXPECT_SOME_EQ("abc", os::read(path::join(dir, "file")));
}
#endif // __WINDOWS__


// TODO(hausdorff): Will not compile until HDFS is supported on Windows. See
// MESOS-5460.
#ifndef __WINDOWS__
TEST_F(HadoopFetcherPluginTest, FetchNonExistingFile)
{
  URI uri = uri::hdfs(path::join(os::getcwd(), "non-exist"));

  uri::fetcher::Flags flags;
  flags.hadoop_client = hadoop;

  Try<Owned<uri::Fetcher>> fetcher = uri::fetcher::create(flags);
  ASSERT_SOME(fetcher);

  string dir = path::join(os::getcwd(), "dir");

  AWAIT_FAILED(fetcher.get()->fetch(uri, dir));
}
#endif // __WINDOWS__


// TODO(hausdorff): Will not compile until HDFS is supported on Windows. See
// MESOS-5460.
#ifndef __WINDOWS__
// This test verifies invoking 'fetch' by plugin name.
TEST_F(HadoopFetcherPluginTest, InvokeFetchByName)
{
  string file = path::join(os::getcwd(), "file");

  ASSERT_SOME(os::write(file, "abc"));

  URI uri = uri::hdfs(file);

  uri::fetcher::Flags flags;
  flags.hadoop_client = hadoop;

  Try<Owned<uri::Fetcher>> fetcher = uri::fetcher::create(flags);
  ASSERT_SOME(fetcher);

  string dir = path::join(os::getcwd(), "dir");

  AWAIT_READY(fetcher.get()->fetch(uri, dir, "hadoop"));

  EXPECT_SOME_EQ("abc", os::read(path::join(dir, "file")));
}
#endif // __WINDOWS__


// TODO(jieyu): Expose this constant so that other docker related
// tests can use this as well.
static constexpr char DOCKER_REGISTRY_HOST[] = "registry-1.docker.io";


class DockerFetcherPluginTest : public TemporaryDirectoryTest {};


TEST_F(DockerFetcherPluginTest, INTERNET_CURL_FetchManifest)
{
  URI uri = uri::docker::manifest(
      "library/busybox",
      "latest",
      DOCKER_REGISTRY_HOST);

  Try<Owned<uri::Fetcher>> fetcher = uri::fetcher::create();
  ASSERT_SOME(fetcher);

  string dir = path::join(os::getcwd(), "dir");

  AWAIT_READY_FOR(fetcher.get()->fetch(uri, dir), Seconds(60));

  Try<string> _manifest = os::read(path::join(dir, "manifest"));
  ASSERT_SOME(_manifest);

  Try<docker::spec::v2::ImageManifest> manifest =
    docker::spec::v2::parse(_manifest.get());

  ASSERT_SOME(manifest);
  EXPECT_EQ("library/busybox", manifest->name());
  EXPECT_EQ("latest", manifest->tag());
}


TEST_F(DockerFetcherPluginTest, INTERNET_CURL_FetchBlob)
{
  const string digest =
    "sha256:a3ed95caeb02ffe68cdd9fd84406680ae93d633cb16422d00e8a7c22955b46d4";

  URI uri = uri::docker::blob(
      "library/busybox",
      digest,
      DOCKER_REGISTRY_HOST);

  Try<Owned<uri::Fetcher>> fetcher = uri::fetcher::create();
  ASSERT_SOME(fetcher);

  string dir = path::join(os::getcwd(), "dir");

  AWAIT_READY_FOR(fetcher.get()->fetch(uri, dir), Seconds(60));

  EXPECT_TRUE(os::exists(path::join(dir, digest)));
}


// Fetches the image manifest and all blobs in that image.
TEST_F(DockerFetcherPluginTest, INTERNET_CURL_FetchImage)
{
  URI uri = uri::docker::image(
      "library/busybox",
      "latest",
      DOCKER_REGISTRY_HOST);

  Try<Owned<uri::Fetcher>> fetcher = uri::fetcher::create();
  ASSERT_SOME(fetcher);

  string dir = path::join(os::getcwd(), "dir");

  AWAIT_READY_FOR(fetcher.get()->fetch(uri, dir), Seconds(60));

  Try<string> _manifest = os::read(path::join(dir, "manifest"));
  ASSERT_SOME(_manifest);

  Try<docker::spec::v2::ImageManifest> manifest =
    docker::spec::v2::parse(_manifest.get());

  ASSERT_SOME(manifest);
  EXPECT_EQ("library/busybox", manifest->name());
  EXPECT_EQ("latest", manifest->tag());

  for (int i = 0; i < manifest->fslayers_size(); i++) {
    EXPECT_TRUE(os::exists(path::join(dir, manifest->fslayers(i).blobsum())));
  }
}


// This test verifies invoking 'fetch' by plugin name.
TEST_F(DockerFetcherPluginTest, INTERNET_CURL_InvokeFetchByName)
{
  URI uri = uri::docker::image(
      "library/busybox",
      "latest",
      DOCKER_REGISTRY_HOST);

  Try<Owned<uri::Fetcher>> fetcher = uri::fetcher::create();
  ASSERT_SOME(fetcher);

  string dir = path::join(os::getcwd(), "dir");

  AWAIT_READY_FOR(fetcher.get()->fetch(uri, dir, "docker"), Seconds(60));

  Try<string> _manifest = os::read(path::join(dir, "manifest"));
  ASSERT_SOME(_manifest);

  Try<docker::spec::v2::ImageManifest> manifest =
    docker::spec::v2::parse(_manifest.get());

  ASSERT_SOME(manifest);
  EXPECT_EQ("library/busybox", manifest->name());
  EXPECT_EQ("latest", manifest->tag());

  for (int i = 0; i < manifest->fslayers_size(); i++) {
    EXPECT_TRUE(os::exists(path::join(dir, manifest->fslayers(i).blobsum())));
  }
}


class CopyFetcherPluginTest : public TemporaryDirectoryTest {};


// Tests CopyFetcher plugin for fetching a valid file.
TEST_F(CopyFetcherPluginTest, FetchExistingFile)
{
  const string file = path::join(os::getcwd(), "file");

  ASSERT_SOME(os::write(file, "abc"));

  // Create a URI for the test file.
  const URI uri = uri::file(file);

  // Use the file fetcher to fetch the URI.
  Try<Owned<uri::Fetcher>> fetcher = uri::fetcher::create();
  ASSERT_SOME(fetcher);

  const string dir = path::join(os::getcwd(), "dir");

  AWAIT_READY(fetcher.get()->fetch(uri, dir));

  // Validate the fetched file's content.
  EXPECT_SOME_EQ("abc", os::read(path::join(dir, "file")));
}


// Negative test case that tests CopyFetcher plugin for a non-exiting file.
TEST_F(CopyFetcherPluginTest, FetchNonExistingFile)
{
  const URI uri = uri::file(path::join(os::getcwd(), "non-exist"));

  // Use the file fetcher to fetch the URI.
  Try<Owned<uri::Fetcher>> fetcher = uri::fetcher::create();
  ASSERT_SOME(fetcher);

  const string dir = path::join(os::getcwd(), "dir");

  // Validate that the fetch failed.
  AWAIT_FAILED(fetcher.get()->fetch(uri, dir));
}


// This test verifies invoking 'fetch' by plugin name.
TEST_F_TEMP_DISABLED_ON_WINDOWS(CopyFetcherPluginTest, InvokeFetchByName)
{
  const string file = path::join(os::getcwd(), "file");

  ASSERT_SOME(os::write(file, "abc"));

  // Create a URI for the test file.
  const URI uri = uri::file(file);

  // Use the file fetcher to fetch the URI.
  Try<Owned<uri::Fetcher>> fetcher = uri::fetcher::create();
  ASSERT_SOME(fetcher);

  const string dir = path::join(os::getcwd(), "dir");

  AWAIT_READY(fetcher.get()->fetch(uri, dir, "copy"));

  // Validate the fetched file's content.
  EXPECT_SOME_EQ("abc", os::read(path::join(dir, "file")));
}

// TODO(jieyu): Add Docker fetcher plugin tests to test with a local
// registry server (w/ or w/o authentication).

} // namespace tests {
} // namespace internal {
} // namespace mesos {
