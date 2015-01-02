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

using namespace mesos;
using namespace mesos::slave;
using namespace mesos::tests;

using namespace process;
using process::Subprocess;
using process::Future;

using slave::Fetcher;

using std::string;
using std::map;

using mesos::fetcher::FetcherInfo;

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
  EXPECT_EQ(flags.hadoop_home, fetcherInfo.get().hadoop_home());
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
  EXPECT_EQ(flags.hadoop_home, fetcherInfo.get().hadoop_home());
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

  EXPECT_EQ(1u, environment.size());

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
  EXPECT_EQ(flags.hadoop_home, fetcherInfo.get().hadoop_home());
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
  EXPECT_EQ(flags.hadoop_home, fetcherInfo.get().hadoop_home());
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
  EXPECT_FALSE(fetcherInfo.get().has_hadoop_home());
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
  EXPECT_EQ(flags.hadoop_home, fetcherInfo.get().hadoop_home());
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
  EXPECT_EQ(flags.hadoop_home, fetcherInfo.get().hadoop_home());
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
  flags.frameworks_home = "/tmp/frameworks";

  CommandInfo commandInfo;
  CommandInfo::URI* uri = commandInfo.add_uris();
  uri->set_value("file://" + testFile);

  map<string, string> environment =
    Fetcher::environment(commandInfo, os::getcwd(), None(), flags);

  Try<Subprocess> fetcherSubprocess =
    process::subprocess(
      path::join(mesos::tests::flags.build_dir, "src/mesos-fetcher"),
      environment);

  ASSERT_SOME(fetcherSubprocess);
  Future<Option<int>> status = fetcherSubprocess.get().status();

  AWAIT_READY(status);
  ASSERT_SOME(status.get());
  EXPECT_EQ(0, status.get().get());

  EXPECT_TRUE(os::exists(localFile));
}


TEST_F(FetcherTest, FilePath)
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
  uri->set_value(testFile);

  map<string, string> environment =
    Fetcher::environment(commandInfo, os::getcwd(), None(), flags);

  Try<Subprocess> fetcherSubprocess =
    process::subprocess(
      path::join(mesos::tests::flags.build_dir, "src/mesos-fetcher"),
      environment);

  ASSERT_SOME(fetcherSubprocess);
  Future<Option<int>> status = fetcherSubprocess.get().status();

  AWAIT_READY(status);
  ASSERT_SOME(status.get());
  EXPECT_EQ(0, status.get().get());

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
      path::join(mesos::tests::flags.build_dir, "src/mesos-fetcher"),
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
      path::join(mesos::tests::flags.build_dir, "src/mesos-fetcher"),
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
