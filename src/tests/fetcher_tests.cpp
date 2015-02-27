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

using namespace process;

using process::Subprocess;
using process::Future;

using mesos::internal::slave::Fetcher;

using std::string;
using std::map;

using mesos::fetcher::FetcherInfo;

namespace mesos {
namespace internal {
namespace tests {

class FetcherEnvironmentTest : public ::testing::Test {};


TEST_F(FetcherEnvironmentTest, Simple)
{
  CommandInfo commandInfo;
  CommandInfo::URI* uri = commandInfo.add_uris();
  uri->set_value("hdfs:///uri");
  uri->set_executable(false);

  string directory = "/tmp/directory";
  Option<string> user = "user";

  slave::Flags flags;
  flags.frameworks_home = "/tmp/frameworks";
  flags.hadoop_home = "/tmp/hadoop";

  map<string, string> environment =
    Fetcher::environment(commandInfo, directory, user, flags);

  EXPECT_EQ(2u, environment.size());

  EXPECT_EQ(flags.hadoop_home, environment["HADOOP_HOME"]);

  Try<JSON::Object> parse =
    JSON::parse<JSON::Object>(environment["MESOS_FETCHER_INFO"]);
  ASSERT_SOME(parse);

  Try<FetcherInfo> fetcherInfo = ::protobuf::parse<FetcherInfo>(parse.get());
  ASSERT_SOME(fetcherInfo);

  EXPECT_EQ(stringify(JSON::Protobuf(commandInfo)),
            stringify(JSON::Protobuf(fetcherInfo.get().command_info())));
  EXPECT_EQ(directory, fetcherInfo.get().work_directory());
  EXPECT_EQ(user.get(), fetcherInfo.get().user());
  EXPECT_EQ(flags.frameworks_home, fetcherInfo.get().frameworks_home());
}


TEST_F(FetcherEnvironmentTest, MultipleURIs)
{
  CommandInfo commandInfo;
  CommandInfo::URI uri;
  uri.set_value("hdfs:///uri1");
  uri.set_executable(false);
  commandInfo.add_uris()->MergeFrom(uri);
  uri.set_value("hdfs:///uri2");
  uri.set_executable(true);
  commandInfo.add_uris()->MergeFrom(uri);

  string directory = "/tmp/directory";
  Option<string> user("user");

  slave::Flags flags;
  flags.frameworks_home = "/tmp/frameworks";
  flags.hadoop_home = "/tmp/hadoop";

  map<string, string> environment =
    Fetcher::environment(commandInfo, directory, user, flags);

  EXPECT_EQ(2u, environment.size());

  EXPECT_EQ(flags.hadoop_home, environment["HADOOP_HOME"]);

  Try<JSON::Object> parse =
    JSON::parse<JSON::Object>(environment["MESOS_FETCHER_INFO"]);
  ASSERT_SOME(parse);

  Try<FetcherInfo> fetcherInfo = ::protobuf::parse<FetcherInfo>(parse.get());
  ASSERT_SOME(fetcherInfo);

  EXPECT_EQ(stringify(JSON::Protobuf(commandInfo)),
            stringify(JSON::Protobuf(fetcherInfo.get().command_info())));
  EXPECT_EQ(directory, fetcherInfo.get().work_directory());
  EXPECT_EQ(user.get(), fetcherInfo.get().user());
  EXPECT_EQ(flags.frameworks_home, fetcherInfo.get().frameworks_home());
}


TEST_F(FetcherEnvironmentTest, NoUser)
{
  CommandInfo commandInfo;
  CommandInfo::URI* uri = commandInfo.add_uris();
  uri->set_value("hdfs:///uri");
  uri->set_executable(false);

  string directory = "/tmp/directory";

  slave::Flags flags;
  flags.frameworks_home = "/tmp/frameworks";
  flags.hadoop_home = "/tmp/hadoop";

  map<string, string> environment =
    Fetcher::environment(commandInfo, directory, None(), flags);

  EXPECT_EQ(2u, environment.size());

  EXPECT_EQ(flags.hadoop_home, environment["HADOOP_HOME"]);

  Try<JSON::Object> parse =
    JSON::parse<JSON::Object>(environment["MESOS_FETCHER_INFO"]);
  ASSERT_SOME(parse);

  Try<FetcherInfo> fetcherInfo = ::protobuf::parse<FetcherInfo>(parse.get());
  ASSERT_SOME(fetcherInfo);

  EXPECT_EQ(stringify(JSON::Protobuf(commandInfo)),
            stringify(JSON::Protobuf(fetcherInfo.get().command_info())));
  EXPECT_EQ(directory, fetcherInfo.get().work_directory());
  EXPECT_FALSE(fetcherInfo.get().has_user());
  EXPECT_EQ(flags.frameworks_home, fetcherInfo.get().frameworks_home());
}


TEST_F(FetcherEnvironmentTest, EmptyHadoop)
{
  CommandInfo commandInfo;
  CommandInfo::URI* uri = commandInfo.add_uris();
  uri->set_value("hdfs:///uri");
  uri->set_executable(false);

  string directory = "/tmp/directory";
  Option<string> user = "user";

  slave::Flags flags;
  flags.frameworks_home = "/tmp/frameworks";
  flags.hadoop_home = "";

  map<string, string> environment =
    Fetcher::environment(commandInfo, directory, user, flags);

  EXPECT_EQ(0u, environment.count("HADOOP_HOME"));
  EXPECT_EQ(1u, environment.size());

  Try<JSON::Object> parse =
    JSON::parse<JSON::Object>(environment["MESOS_FETCHER_INFO"]);
  ASSERT_SOME(parse);

  Try<FetcherInfo> fetcherInfo = ::protobuf::parse<FetcherInfo>(parse.get());
  ASSERT_SOME(fetcherInfo);

  EXPECT_EQ(stringify(JSON::Protobuf(commandInfo)),
            stringify(JSON::Protobuf(fetcherInfo.get().command_info())));
  EXPECT_EQ(directory, fetcherInfo.get().work_directory());
  EXPECT_EQ(user.get(), fetcherInfo.get().user());
  EXPECT_EQ(flags.frameworks_home, fetcherInfo.get().frameworks_home());
}


TEST_F(FetcherEnvironmentTest, NoHadoop)
{
  CommandInfo commandInfo;
  CommandInfo::URI* uri = commandInfo.add_uris();
  uri->set_value("hdfs:///uri");
  uri->set_executable(false);

  string directory = "/tmp/directory";
  Option<string> user = "user";

  slave::Flags flags;
  flags.frameworks_home = "/tmp/frameworks";

  map<string, string> environment =
    Fetcher::environment(commandInfo, directory, user, flags);

  EXPECT_EQ(0u, environment.count("HADOOP_HOME"));
  EXPECT_EQ(1u, environment.size());

  Try<JSON::Object> parse =
    JSON::parse<JSON::Object>(environment["MESOS_FETCHER_INFO"]);
  ASSERT_SOME(parse);

  Try<FetcherInfo> fetcherInfo = ::protobuf::parse<FetcherInfo>(parse.get());
  ASSERT_SOME(fetcherInfo);

  EXPECT_EQ(stringify(JSON::Protobuf(commandInfo)),
            stringify(JSON::Protobuf(fetcherInfo.get().command_info())));
  EXPECT_EQ(directory, fetcherInfo.get().work_directory());
  EXPECT_EQ(user.get(), fetcherInfo.get().user());
  EXPECT_EQ(flags.frameworks_home, fetcherInfo.get().frameworks_home());
}


TEST_F(FetcherEnvironmentTest, NoExtractNoExecutable)
{
  CommandInfo commandInfo;
  CommandInfo::URI* uri = commandInfo.add_uris();
  uri->set_value("hdfs:///uri");
  uri->set_executable(false);
  uri->set_extract(false);

  string directory = "/tmp/directory";
  Option<string> user = "user";

  slave::Flags flags;
  flags.frameworks_home = "/tmp/frameworks";
  flags.hadoop_home = "/tmp/hadoop";

  map<string, string> environment =
    Fetcher::environment(commandInfo, directory, user, flags);

  EXPECT_EQ(2u, environment.size());

  EXPECT_EQ(flags.hadoop_home, environment["HADOOP_HOME"]);

  Try<JSON::Object> parse =
    JSON::parse<JSON::Object>(environment["MESOS_FETCHER_INFO"]);
  ASSERT_SOME(parse);

  Try<FetcherInfo> fetcherInfo = ::protobuf::parse<FetcherInfo>(parse.get());
  ASSERT_SOME(fetcherInfo);

  EXPECT_EQ(stringify(JSON::Protobuf(commandInfo)),
            stringify(JSON::Protobuf(fetcherInfo.get().command_info())));
  EXPECT_EQ(directory, fetcherInfo.get().work_directory());
  EXPECT_EQ(user.get(), fetcherInfo.get().user());
  EXPECT_EQ(flags.frameworks_home, fetcherInfo.get().frameworks_home());
}


TEST_F(FetcherEnvironmentTest, NoExtractExecutable)
{
  CommandInfo commandInfo;
  CommandInfo::URI* uri = commandInfo.add_uris();
  uri->set_value("hdfs:///uri");
  uri->set_executable(true);
  uri->set_extract(false);

  string directory = "/tmp/directory";
  Option<string> user = "user";

  slave::Flags flags;
  flags.frameworks_home = "/tmp/frameworks";
  flags.hadoop_home = "/tmp/hadoop";

  map<string, string> environment =
    Fetcher::environment(commandInfo, directory, user, flags);

  EXPECT_EQ(2u, environment.size());

  EXPECT_EQ(flags.hadoop_home, environment["HADOOP_HOME"]);

  Try<JSON::Object> parse =
    JSON::parse<JSON::Object>(environment["MESOS_FETCHER_INFO"]);
  ASSERT_SOME(parse);

  Try<FetcherInfo> fetcherInfo = ::protobuf::parse<FetcherInfo>(parse.get());
  ASSERT_SOME(fetcherInfo);

  EXPECT_EQ(stringify(JSON::Protobuf(commandInfo)),
            stringify(JSON::Protobuf(fetcherInfo.get().command_info())));
  EXPECT_EQ(directory, fetcherInfo.get().work_directory());
  EXPECT_EQ(user.get(), fetcherInfo.get().user());
  EXPECT_EQ(flags.frameworks_home, fetcherInfo.get().frameworks_home());
}


class FetcherTest : public TemporaryDirectoryTest {};


TEST_F(FetcherTest, FileURI)
{
  string fromDir = path::join(os::getcwd(), "from");
  ASSERT_SOME(os::mkdir(fromDir));
  string testFile = path::join(fromDir, "test");
  EXPECT_FALSE(os::write(testFile, "data").isError());

  string localFile = path::join(os::getcwd(), "test");
  EXPECT_FALSE(os::exists(localFile));

  slave::Flags flags;

  CommandInfo commandInfo;
  CommandInfo::URI* uri = commandInfo.add_uris();
  uri->set_value("file://" + testFile);

  map<string, string> environment =
    Fetcher::environment(commandInfo, os::getcwd(), None(), flags);

  Try<Subprocess> fetcherSubprocess =
    process::subprocess(
      path::join(mesos::internal::tests::flags.build_dir, "src/mesos-fetcher"),
      environment);

  ASSERT_SOME(fetcherSubprocess);
  Future<Option<int>> status = fetcherSubprocess.get().status();

  AWAIT_READY(status);
  ASSERT_SOME(status.get());
  EXPECT_EQ(0, status.get().get());

  EXPECT_TRUE(os::exists(localFile));
}


TEST_F(FetcherTest, AbsoluteFilePath)
{
  string fromDir = path::join(os::getcwd(), "from");
  ASSERT_SOME(os::mkdir(fromDir));
  string testPath = path::join(fromDir, "test");
  EXPECT_FALSE(os::write(testPath, "data").isError());

  string localFile = path::join(os::getcwd(), "test");
  EXPECT_FALSE(os::exists(localFile));

  slave::Flags flags;

  CommandInfo commandInfo;
  CommandInfo::URI* uri = commandInfo.add_uris();
  uri->set_value(testPath);

  map<string, string> environment =
    Fetcher::environment(commandInfo, os::getcwd(), None(), flags);

  Try<Subprocess> fetcherSubprocess =
    process::subprocess(
      path::join(mesos::internal::tests::flags.build_dir, "src/mesos-fetcher"),
      environment);

  ASSERT_SOME(fetcherSubprocess);
  Future<Option<int>> status = fetcherSubprocess.get().status();

  AWAIT_READY(status);
  ASSERT_SOME(status.get());
  EXPECT_EQ(0, status.get().get());

  EXPECT_TRUE(os::exists(localFile));
}


TEST_F(FetcherTest, RelativeFilePath)
{
  string fromDir = path::join(os::getcwd(), "from");
  ASSERT_SOME(os::mkdir(fromDir));
  string testPath = path::join(fromDir, "test");
  EXPECT_FALSE(os::write(testPath, "data").isError());

  string localFile = path::join(os::getcwd(), "test");
  EXPECT_FALSE(os::exists(localFile));

  slave::Flags flags;

  CommandInfo commandInfo;
  CommandInfo::URI* uri = commandInfo.add_uris();
  uri->set_value("test");

  // The first run must fail, because we have not set frameworks_home yet.

  map<string, string> environment1 =
    Fetcher::environment(commandInfo, os::getcwd(), None(), flags);

  Try<Subprocess> fetcherSubprocess1 =
    process::subprocess(
      path::join(mesos::internal::tests::flags.build_dir, "src/mesos-fetcher"),
      environment1);

  ASSERT_SOME(fetcherSubprocess1);
  Future<Option<int>> status1 = fetcherSubprocess1.get().status();

  AWAIT_READY(status1);
  ASSERT_SOME(status1.get());

  // mesos-fetcher always exits with EXIT(1) on failure.
  EXPECT_EQ(1, WIFEXITED(status1.get().get()));

  EXPECT_FALSE(os::exists(localFile));

  // The next run must succeed due to this flag.
  flags.frameworks_home = fromDir;

  map<string, string> environment2 =
    Fetcher::environment(commandInfo, os::getcwd(), None(), flags);

  Try<Subprocess> fetcherSubprocess2 =
    process::subprocess(
      path::join(mesos::internal::tests::flags.build_dir, "src/mesos-fetcher"),
      environment2);

  ASSERT_SOME(fetcherSubprocess2);
  Future<Option<int>> status2 = fetcherSubprocess2.get().status();

  AWAIT_READY(status2);
  ASSERT_SOME(status2.get());
  EXPECT_EQ(0, status2.get().get());

  EXPECT_TRUE(os::exists(localFile));
}


class HttpProcess : public Process<HttpProcess>
{
public:
  HttpProcess()
  {
    route("/help", None(), &HttpProcess::index);
  }

  Future<http::Response> index(const http::Request& request)
  {
    return http::OK();
  }
};


TEST_F(FetcherTest, OSNetUriTest)
{
  HttpProcess process;

  spawn(process);

  string url = "http://" + net::getHostname(process.self().address.ip).get() +
                ":" + stringify(process.self().address.port) + "/help";

  string localFile = path::join(os::getcwd(), "help");
  EXPECT_FALSE(os::exists(localFile));

  slave::Flags flags;
  flags.frameworks_home = "/tmp/frameworks";

  CommandInfo commandInfo;
  CommandInfo::URI* uri = commandInfo.add_uris();
  uri->set_value(url);

  map<string, string> environment =
    Fetcher::environment(commandInfo, os::getcwd(), None(), flags);

  Try<Subprocess> fetcherSubprocess =
    process::subprocess(
      path::join(mesos::internal::tests::flags.build_dir, "src/mesos-fetcher"),
      environment);

  ASSERT_SOME(fetcherSubprocess);
  Future<Option<int>> status = fetcherSubprocess.get().status();

  AWAIT_READY(status);
  ASSERT_SOME(status.get());
  EXPECT_EQ(0, status.get().get());

  EXPECT_TRUE(os::exists(localFile));
}


TEST_F(FetcherTest, FileLocalhostURI)
{
  string fromDir = path::join(os::getcwd(), "from");
  ASSERT_SOME(os::mkdir(fromDir));
  string testFile = path::join(fromDir, "test");
  EXPECT_FALSE(os::write(testFile, "data").isError());

  string localFile = path::join(os::getcwd(), "test");
  EXPECT_FALSE(os::exists(localFile));

  slave::Flags flags;
  flags.frameworks_home = "/tmp/frameworks";

  CommandInfo commandInfo;
  CommandInfo::URI* uri = commandInfo.add_uris();
  uri->set_value(path::join("file://localhost", testFile));

  map<string, string> environment =
    Fetcher::environment(commandInfo, os::getcwd(), None(), flags);

  Try<Subprocess> fetcherSubprocess =
    process::subprocess(
      path::join(mesos::internal::tests::flags.build_dir, "src/mesos-fetcher"),
      environment);

  ASSERT_SOME(fetcherSubprocess);
  Future<Option<int>> status = fetcherSubprocess.get().status();

  AWAIT_READY(status);
  ASSERT_SOME(status.get());
  EXPECT_EQ(0, status.get().get());

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

  Option<int> stdout = None();
  Option<int> stderr = None();

  // Redirect mesos-fetcher output if running the tests verbosely.
  if (tests::flags.verbose) {
    stdout = STDOUT_FILENO;
    stderr = STDERR_FILENO;
  }

  Fetcher fetcher;

  Future<Nothing> fetch = fetcher.fetch(
      containerId, commandInfo, os::getcwd(), None(), flags, stdout, stderr);

  AWAIT_READY(fetch);

  Try<string> basename = os::basename(path.get());

  ASSERT_SOME(basename);

  Try<os::Permissions> permissions = os::permissions(basename.get());

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

  Option<int> stdout = None();
  Option<int> stderr = None();

  // Redirect mesos-fetcher output if running the tests verbosely.
  if (tests::flags.verbose) {
    stdout = STDOUT_FILENO;
    stderr = STDERR_FILENO;
  }

  Fetcher fetcher;

  Future<Nothing> fetch = fetcher.fetch(
      containerId, commandInfo, os::getcwd(), None(), flags, stdout, stderr);

  AWAIT_READY(fetch);

  Try<string> basename = os::basename(path.get());

  ASSERT_SOME(basename);

  Try<os::Permissions> permissions = os::permissions(basename.get());

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

  Option<int> stdout = None();
  Option<int> stderr = None();

  // Redirect mesos-fetcher output if running the tests verbosely.
  if (tests::flags.verbose) {
    stdout = STDOUT_FILENO;
    stderr = STDERR_FILENO;
  }

  Fetcher fetcher;

  Future<Nothing> fetch = fetcher.fetch(
      containerId, commandInfo, os::getcwd(), None(), flags, stdout, stderr);

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

  // This acts exactly as "hadoop" for testing purposes.
  string mockHadoopScript =
    "#!/usr/bin/env bash\n"
    "\n"
    "touch " + proof + "\n"
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

  Option<int> stdout = None();
  Option<int> stderr = None();

  // Redirect mesos-fetcher output if running the tests verbosely.
  if (tests::flags.verbose) {
    stdout = STDOUT_FILENO;
    stderr = STDERR_FILENO;
  }

  Fetcher fetcher;

  Future<Nothing> fetch = fetcher.fetch(
      containerId, commandInfo, os::getcwd(), None(), flags, stdout, stderr);

  AWAIT_READY(fetch);

  // Proof that we used our own mock version of Hadoop.
  ASSERT_TRUE(os::exists(proof));

  // Proof that hdfs fetching worked.
  EXPECT_TRUE(os::exists(localFile));
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
