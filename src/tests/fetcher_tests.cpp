/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <unistd.h>

#include <map>
#include <string>

#include <hdfs/hdfs.hpp>

#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/http.hpp>
#include <process/subprocess.hpp>

#include <stout/gtest.hpp>
#include <stout/json.hpp>
#include <stout/net.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/protobuf.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

#include <mesos/fetcher/fetcher.hpp>

#include "slave/containerizer/fetcher.hpp"
#include "slave/flags.hpp"

#include "tests/environment.hpp"
#include "tests/flags.hpp"
#include "tests/mesos.hpp"
#include "tests/utils.hpp"

using namespace mesos::slave;

using namespace process;

using mesos::fetcher::FetcherInfo;

using mesos::internal::slave::Fetcher;

using process::Subprocess;
using process::Future;

using std::map;
using std::string;


namespace mesos {
namespace internal {
namespace tests {

class FetcherTest : public TemporaryDirectoryTest {};


TEST_F(FetcherTest, FileURI)
{
  string fromDir = path::join(os::getcwd(), "from");
  ASSERT_SOME(os::mkdir(fromDir));
  string testFile = path::join(fromDir, "test");
  EXPECT_SOME(os::write(testFile, "data"));

  string localFile = path::join(os::getcwd(), "test");
  EXPECT_FALSE(os::exists(localFile));

  slave::Flags flags;
  flags.launcher_dir = path::join(tests::flags.build_dir, "src");

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  CommandInfo commandInfo;
  CommandInfo::URI* uri = commandInfo.add_uris();
  uri->set_value("file://" + testFile);

  Fetcher fetcher;
  SlaveID slaveId;

  Future<Nothing> fetch = fetcher.fetch(
      containerId, commandInfo, os::getcwd(), None(), slaveId, flags);
  AWAIT_READY(fetch);

  EXPECT_TRUE(os::exists(localFile));
}


// Negative test: invalid user name. Copied from FileTest, so this
// normally would succeed, but here a bogus user name is specified.
// So we check for fetch failure.
TEST_F(FetcherTest, InvalidUser)
{
  string fromDir = path::join(os::getcwd(), "from");
  ASSERT_SOME(os::mkdir(fromDir));
  string testFile = path::join(fromDir, "test");
  EXPECT_SOME(os::write(testFile, "data"));

  string localFile = path::join(os::getcwd(), "test");
  EXPECT_FALSE(os::exists(localFile));

  slave::Flags flags;
  flags.launcher_dir = path::join(tests::flags.build_dir, "src");
  flags.frameworks_home = "/tmp/frameworks";

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  CommandInfo commandInfo;
  commandInfo.set_user(UUID::random().toString());

  CommandInfo::URI* uri = commandInfo.add_uris();
  uri->set_value("file://" + testFile);

  Fetcher fetcher;
  SlaveID slaveId;

  Future<Nothing> fetch = fetcher.fetch(
      containerId, commandInfo, os::getcwd(), None(), slaveId, flags);
  AWAIT_FAILED(fetch);

  // See FetcherProcess::fetch(), the message must mention "chown" in
  // this case.
  EXPECT_TRUE(strings::contains(fetch.failure(), "chown"));

  EXPECT_FALSE(os::exists(localFile));
}


// Negative test: URI leading to non-existing file. Copied from FileTest,
// but here the resource is missing. So we check for fetch failure.
TEST_F(FetcherTest, NonExistingFile)
{
  string fromDir = path::join(os::getcwd(), "from");
  ASSERT_SOME(os::mkdir(fromDir));
  string testFile = path::join(fromDir, "nonExistingFile");

  slave::Flags flags;
  flags.launcher_dir = path::join(tests::flags.build_dir, "src");
  flags.frameworks_home = "/tmp/frameworks";

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  CommandInfo commandInfo;
  CommandInfo::URI* uri = commandInfo.add_uris();
  uri->set_value("file://" + testFile);

  Fetcher fetcher;
  SlaveID slaveId;

  Future<Nothing> fetch = fetcher.fetch(
      containerId, commandInfo, os::getcwd(), None(), slaveId, flags);
  AWAIT_FAILED(fetch);

  // See FetcherProcess::run().
  EXPECT_TRUE(strings::contains(fetch.failure(), "Failed to fetch"));
}


// Negative test: malformed URI, missing path.
TEST_F(FetcherTest, MalformedURI)
{
  slave::Flags flags;
  flags.launcher_dir = path::join(tests::flags.build_dir, "src");
  flags.frameworks_home = "/tmp/frameworks";

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  CommandInfo commandInfo;
  CommandInfo::URI* uri = commandInfo.add_uris();
  uri->set_value("lala://nopath");

  Fetcher fetcher;
  SlaveID slaveId;

  Future<Nothing> fetch = fetcher.fetch(
      containerId, commandInfo, os::getcwd(), None(), slaveId, flags);
  AWAIT_FAILED(fetch);

  // See Fetcher::basename().
  EXPECT_TRUE(strings::contains(fetch.failure(), "Malformed"));
}


TEST_F(FetcherTest, AbsoluteFilePath)
{
  string fromDir = path::join(os::getcwd(), "from");
  ASSERT_SOME(os::mkdir(fromDir));
  string testPath = path::join(fromDir, "test");
  EXPECT_SOME(os::write(testPath, "data"));

  string localFile = path::join(os::getcwd(), "test");
  EXPECT_FALSE(os::exists(localFile));

  slave::Flags flags;
  flags.launcher_dir = path::join(tests::flags.build_dir, "src");

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  CommandInfo commandInfo;
  CommandInfo::URI* uri = commandInfo.add_uris();
  uri->set_value(testPath);

  Fetcher fetcher;
  SlaveID slaveId;

  Future<Nothing> fetch = fetcher.fetch(
      containerId, commandInfo, os::getcwd(), None(), slaveId, flags);
  AWAIT_READY(fetch);

  EXPECT_TRUE(os::exists(localFile));
}


TEST_F(FetcherTest, RelativeFilePath)
{
  string fromDir = path::join(os::getcwd(), "from");
  ASSERT_SOME(os::mkdir(fromDir));
  string testPath = path::join(fromDir, "test");
  EXPECT_SOME(os::write(testPath, "data"));

  string localFile = path::join(os::getcwd(), "test");
  EXPECT_FALSE(os::exists(localFile));

  slave::Flags flags;
  flags.launcher_dir = path::join(tests::flags.build_dir, "src");

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  CommandInfo commandInfo;
  CommandInfo::URI* uri = commandInfo.add_uris();
  uri->set_value("test");

  Fetcher fetcher;
  SlaveID slaveId;

  // The first run must fail, because we have not set frameworks_home yet.

  Future<Nothing> fetch1 = fetcher.fetch(
      containerId, commandInfo, os::getcwd(), None(), slaveId, flags);
  AWAIT_FAILED(fetch1);

  EXPECT_FALSE(os::exists(localFile));

  // The next run must succeed due to this flag.
  flags.frameworks_home = fromDir;

  Future<Nothing> fetch2 = fetcher.fetch(
      containerId, commandInfo, os::getcwd(), None(), slaveId, flags);
  AWAIT_READY(fetch2);

  EXPECT_TRUE(os::exists(localFile));
}


class HttpProcess : public Process<HttpProcess>
{
public:
  HttpProcess()
  {
    route("/test", None(), &HttpProcess::test);
  }

  MOCK_METHOD1(test, Future<http::Response>(const http::Request&));
};


TEST_F(FetcherTest, OSNetUriTest)
{
  HttpProcess process;

  spawn(process);

  const network::Address& address = process.self().address;

  process::http::URL url(
      "http",
      address.ip,
      address.port,
      path::join(process.self().id, "test"));

  string localFile = path::join(os::getcwd(), "test");
  EXPECT_FALSE(os::exists(localFile));

  slave::Flags flags;
  flags.launcher_dir = path::join(tests::flags.build_dir, "src");
  flags.frameworks_home = "/tmp/frameworks";

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  CommandInfo commandInfo;
  CommandInfo::URI* uri = commandInfo.add_uris();
  uri->set_value(stringify(url));

  Fetcher fetcher;
  SlaveID slaveId;

  EXPECT_CALL(process, test(_))
    .WillOnce(Return(http::OK()));

  Future<Nothing> fetch = fetcher.fetch(
      containerId, commandInfo, os::getcwd(), None(), slaveId, flags);

  AWAIT_READY(fetch);

  EXPECT_TRUE(os::exists(localFile));
}


// Tests whether fetcher can process URIs that contain leading whitespace
// characters. This was added as a verification for MESOS-2862.
//
// TODO(hartem): This test case should be merged with the previous one.
TEST_F(FetcherTest, OSNetUriSpaceTest)
{
  HttpProcess process;

  spawn(process);

  const network::Address& address = process.self().address;

  process::http::URL url(
      "http",
      address.ip,
      address.port,
      path::join(process.self().id, "test"));

  string localFile = path::join(os::getcwd(), "test");
  EXPECT_FALSE(os::exists(localFile));

  slave::Flags flags;
  flags.launcher_dir = path::join(tests::flags.build_dir, "src");
  flags.frameworks_home = "/tmp/frameworks";

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  CommandInfo commandInfo;
  CommandInfo::URI* uri = commandInfo.add_uris();

  // Add whitespace characters to the beginning of the URL.
  uri->set_value("\r\n\t " + stringify(url));

  Fetcher fetcher;
  SlaveID slaveId;

  // Verify that the intended endpoint is hit.
  EXPECT_CALL(process, test(_))
    .WillOnce(Return(http::OK()));

  Future<Nothing> fetch = fetcher.fetch(
      containerId, commandInfo, os::getcwd(), None(), slaveId, flags);

  AWAIT_READY(fetch);

  EXPECT_TRUE(os::exists(localFile));
}


TEST_F(FetcherTest, FileLocalhostURI)
{
  string fromDir = path::join(os::getcwd(), "from");
  ASSERT_SOME(os::mkdir(fromDir));
  string testFile = path::join(fromDir, "test");
  EXPECT_SOME(os::write(testFile, "data"));

  string localFile = path::join(os::getcwd(), "test");
  EXPECT_FALSE(os::exists(localFile));

  slave::Flags flags;
  flags.launcher_dir = path::join(tests::flags.build_dir, "src");

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  CommandInfo commandInfo;
  CommandInfo::URI* uri = commandInfo.add_uris();
  uri->set_value(path::join("file://localhost", testFile));

  Fetcher fetcher;
  SlaveID slaveId;

  Future<Nothing> fetch = fetcher.fetch(
      containerId, commandInfo, os::getcwd(), None(), slaveId, flags);
  AWAIT_READY(fetch);

  EXPECT_TRUE(os::exists(localFile));
}


TEST_F(FetcherTest, NoExtractNotExecutable)
{
  // First construct a temporary file that can be fetched.
  Try<string> path = os::mktemp();

  ASSERT_SOME(path);

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  CommandInfo commandInfo;
  CommandInfo::URI* uri = commandInfo.add_uris();
  uri->set_value(path.get());
  uri->set_executable(false);
  uri->set_extract(false);

  slave::Flags flags;
  flags.launcher_dir = path::join(tests::flags.build_dir, "src");

  Fetcher fetcher;
  SlaveID slaveId;

  Future<Nothing> fetch = fetcher.fetch(
      containerId, commandInfo, os::getcwd(), None(), slaveId, flags);
  AWAIT_READY(fetch);

  string basename = Path(path.get()).basename();

  Try<os::Permissions> permissions = os::permissions(basename);

  ASSERT_SOME(permissions);
  EXPECT_FALSE(permissions.get().owner.x);
  EXPECT_FALSE(permissions.get().group.x);
  EXPECT_FALSE(permissions.get().others.x);

  ASSERT_SOME(os::rm(path.get()));
}


TEST_F(FetcherTest, NoExtractExecutable)
{
  // First construct a temporary file that can be fetched.
  Try<string> path = os::mktemp();

  ASSERT_SOME(path);

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  CommandInfo commandInfo;
  CommandInfo::URI* uri = commandInfo.add_uris();
  uri->set_value(path.get());
  uri->set_executable(true);
  uri->set_extract(false);

  slave::Flags flags;
  flags.launcher_dir = path::join(tests::flags.build_dir, "src");

  Fetcher fetcher;
  SlaveID slaveId;

  Future<Nothing> fetch = fetcher.fetch(
      containerId, commandInfo, os::getcwd(), None(), slaveId, flags);

  AWAIT_READY(fetch);

  string basename = Path(path.get()).basename();

  Try<os::Permissions> permissions = os::permissions(basename);

  ASSERT_SOME(permissions);
  EXPECT_TRUE(permissions.get().owner.x);
  EXPECT_TRUE(permissions.get().group.x);
  EXPECT_TRUE(permissions.get().others.x);

  ASSERT_SOME(os::rm(path.get()));
}


TEST_F(FetcherTest, ExtractNotExecutable)
{
  // First construct a temporary file that can be fetched and archive
  // with tar  gzip.
  Try<string> path = os::mktemp();

  ASSERT_SOME(path);

  ASSERT_SOME(os::write(path.get(), "hello world"));

  // TODO(benh): Update os::tar so that we can capture or ignore
  // stdout/stderr output.

  ASSERT_SOME(os::tar(path.get(), path.get() + ".tar.gz"));

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  CommandInfo commandInfo;
  CommandInfo::URI* uri = commandInfo.add_uris();
  uri->set_value(path.get() + ".tar.gz");
  uri->set_executable(false);
  uri->set_extract(true);

  slave::Flags flags;
  flags.launcher_dir = path::join(tests::flags.build_dir, "src");

  Fetcher fetcher;
  SlaveID slaveId;

  Future<Nothing> fetch = fetcher.fetch(
      containerId, commandInfo, os::getcwd(), None(), slaveId, flags);

  AWAIT_READY(fetch);

  ASSERT_TRUE(os::exists(path::join(".", path.get())));

  ASSERT_SOME_EQ("hello world", os::read(path::join(".", path.get())));

  Try<os::Permissions> permissions =
    os::permissions(path::join(".", path.get()));

  ASSERT_SOME(permissions);
  EXPECT_FALSE(permissions.get().owner.x);
  EXPECT_FALSE(permissions.get().group.x);
  EXPECT_FALSE(permissions.get().others.x);

  ASSERT_SOME(os::rm(path.get()));
}


// Tests fetching via the local HDFS client. Since we cannot rely on
// Hadoop being installed, we use our own mock version that works on
// the local file system only, but this lets us exercise the exact
// same C++ code paths as if there were real Hadoop at the other end.
// Specifying a source URI that begins with "hdfs://" makes control
// flow go there. To use this method of fetching, the slave flag
// hadoop_home gets set to a path where we create a fake hadoop
// executable. This also tests whether the HADOOP_HOME environment
// variable gets set for mesos-fetcher (MESOS-2390).
TEST_F(FetcherTest, HdfsURI)
{
  string hadoopPath = os::getcwd();
  ASSERT_TRUE(os::exists(hadoopPath));

  string hadoopBinPath = path::join(hadoopPath, "bin");
  ASSERT_SOME(os::mkdir(hadoopBinPath));
  ASSERT_SOME(os::chmod(hadoopBinPath, S_IRWXU | S_IRWXG | S_IRWXO));

  const string& proof = path::join(hadoopPath, "proof");

  // This acts exactly as "hadoop" for testing purposes. On some platforms, the
  // "hadoop" wrapper command will emit a warning that Hadoop installation has
  // no native code support. We always emit that here to make sure it is parsed
  // correctly.
  string mockHadoopScript =
    "#!/usr/bin/env bash\n"
    "\n"
    "touch " + proof + "\n"
    "\n"
    "now=$(date '+%y/%m/%d %I:%M:%S')\n"
    "echo \"$now WARN util.NativeCodeLoader: "
      "Unable to load native-hadoop library for your platform...\" 1>&2\n"
    "\n"
    "if [[ 'version' == $1 ]]; then\n"
    "  echo $0 'for Mesos testing'\n"
    "fi\n"
    "\n"
    "# hadoop fs -copyToLocal $3 $4\n"
    "if [[ 'fs' == $1 && '-copyToLocal' == $2 ]]; then\n"
    "  if [[ $3 == 'hdfs://'* ]]; then\n"
    "    # Remove 'hdfs://<host>/' and use just the (absolute) path.\n"
    "    withoutProtocol=${3/'hdfs:'\\/\\//}\n"
    "    withoutHost=${withoutProtocol#*\\/}\n"
    "    absolutePath='/'$withoutHost\n"
    "   cp $absolutePath $4\n"
    "  else\n"
    "    cp #3 $4\n"
    "  fi\n"
    "fi\n";

  string hadoopCommand = path::join(hadoopBinPath, "hadoop");
  ASSERT_SOME(os::write(hadoopCommand, mockHadoopScript));
  ASSERT_SOME(os::chmod(hadoopCommand,
                        S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH));

  string fromDir = path::join(os::getcwd(), "from");
  ASSERT_SOME(os::mkdir(fromDir));
  string testFile = path::join(fromDir, "test");
  EXPECT_SOME(os::write(testFile, "data"));

  string localFile = path::join(os::getcwd(), "test");
  EXPECT_FALSE(os::exists(localFile));

  slave::Flags flags;
  flags.launcher_dir = path::join(tests::flags.build_dir, "src");
  flags.hadoop_home = hadoopPath;

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  CommandInfo commandInfo;
  CommandInfo::URI* uri = commandInfo.add_uris();
  uri->set_value(path::join("hdfs://localhost", testFile));

  Fetcher fetcher;
  SlaveID slaveId;

  Future<Nothing> fetch = fetcher.fetch(
      containerId, commandInfo, os::getcwd(), None(), slaveId, flags);

  AWAIT_READY(fetch);

  // Proof that we used our own mock version of Hadoop.
  ASSERT_TRUE(os::exists(proof));

  // Proof that hdfs fetching worked.
  EXPECT_TRUE(os::exists(localFile));
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
