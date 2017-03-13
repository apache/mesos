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

#ifndef __WINDOWS__
#include <unistd.h>
#endif // __WINDOWS__

#include <map>
#include <string>

#include <hdfs/hdfs.hpp>

#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/http.hpp>
#include <process/subprocess.hpp>

#include <stout/base64.hpp>
#include <stout/gtest.hpp>
#include <stout/net.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

#include <stout/os/permissions.hpp>

#include <mesos/fetcher/fetcher.hpp>
#include <mesos/type_utils.hpp>

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


TEST_F_TEMP_DISABLED_ON_WINDOWS(FetcherTest, FileURI)
{
  string fromDir = path::join(os::getcwd(), "from");
  ASSERT_SOME(os::mkdir(fromDir));
  string testFile = path::join(fromDir, "test");
  EXPECT_SOME(os::write(testFile, "data"));

  string localFile = path::join(os::getcwd(), "test");
  EXPECT_FALSE(os::exists(localFile));

  slave::Flags flags;
  flags.launcher_dir = getLauncherDir();

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


// TODO(hausdorff): `os::getuid` does not exist on Windows.
#ifndef __WINDOWS__
// Tests that non-root users are unable to fetch root-protected files on the
// local filesystem.
TEST_F_TEMP_DISABLED_ON_WINDOWS(FetcherTest, ROOT_RootProtectedFileURI)
{
  const string user = "nobody";
  ASSERT_SOME(os::getuid(user));

  string fromDir = path::join(os::getcwd(), "from");
  ASSERT_SOME(os::mkdir(fromDir));
  string testFile = path::join(fromDir, "test");
  EXPECT_SOME(os::write(testFile, "data"));
  EXPECT_SOME(os::chmod(testFile, 600));

  slave::Flags flags;
  flags.launcher_dir = getLauncherDir();

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  CommandInfo commandInfo;
  commandInfo.set_user(user);

  CommandInfo::URI* uri = commandInfo.add_uris();
  uri->set_value("file://" + testFile);

  Fetcher fetcher;
  SlaveID slaveId;

  AWAIT_FAILED(fetcher.fetch(
      containerId,
      commandInfo,
      os::getcwd(),
      None(),
      slaveId,
      flags));
}
#endif // __WINDOWS__


TEST_F_TEMP_DISABLED_ON_WINDOWS(FetcherTest, CustomOutputFileSubdirectory)
{
  string testFile = path::join(os::getcwd(), "test");
  EXPECT_SOME(os::write(testFile, "data"));

  string customOutputFile = "subdir/custom.txt";
  string localFile = path::join(os::getcwd(), customOutputFile);
  EXPECT_FALSE(os::exists(localFile));

  slave::Flags flags;
  flags.launcher_dir = getLauncherDir();

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  CommandInfo commandInfo;
  CommandInfo::URI* uri = commandInfo.add_uris();
  uri->set_value("file://" + testFile);
  uri->set_output_file(customOutputFile);

  Fetcher fetcher;
  SlaveID slaveId;

  Future<Nothing> fetch = fetcher.fetch(
      containerId, commandInfo, os::getcwd(), None(), slaveId, flags);
  AWAIT_READY(fetch);

  EXPECT_TRUE(os::exists(localFile));
}


// Negative test: invalid custom URI output file. If the user specifies a
// path for the file saved in the sandbox that has a directory component,
// it must be a relative path.
TEST_F_TEMP_DISABLED_ON_WINDOWS(FetcherTest, AbsoluteCustomSubdirectoryFails)
{
  string fromDir = path::join(os::getcwd(), "from");
  ASSERT_SOME(os::mkdir(fromDir));
  string testFile = path::join(fromDir, "test");
  EXPECT_SOME(os::write(testFile, "data"));

  string customOutputFile = "/subdir/custom.txt";
  string localFile = path::join(os::getcwd(), customOutputFile);
  EXPECT_FALSE(os::exists(localFile));

  slave::Flags flags;
  flags.launcher_dir = getLauncherDir();

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  CommandInfo commandInfo;
  CommandInfo::URI* uri = commandInfo.add_uris();
  uri->set_value("file://" + testFile);
  uri->set_output_file(customOutputFile);

  Fetcher fetcher;
  SlaveID slaveId;

  Future<Nothing> fetch = fetcher.fetch(
      containerId, commandInfo, os::getcwd(), None(), slaveId, flags);
  AWAIT_FAILED(fetch);

  EXPECT_FALSE(os::exists(localFile));
}


// Negative test: invalid user name. Copied from FileTest, so this
// normally would succeed, but here a bogus user name is specified.
// So we check for fetch failure.
TEST_F_TEMP_DISABLED_ON_WINDOWS(FetcherTest, InvalidUser)
{
  string fromDir = path::join(os::getcwd(), "from");
  ASSERT_SOME(os::mkdir(fromDir));
  string testFile = path::join(fromDir, "test");
  EXPECT_SOME(os::write(testFile, "data"));

  string localFile = path::join(os::getcwd(), "test");
  EXPECT_FALSE(os::exists(localFile));

  slave::Flags flags;
  flags.launcher_dir = getLauncherDir();
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
TEST_F_TEMP_DISABLED_ON_WINDOWS(FetcherTest, NonExistingFile)
{
  string fromDir = path::join(os::getcwd(), "from");
  ASSERT_SOME(os::mkdir(fromDir));
  string testFile = path::join(fromDir, "nonExistingFile");

  slave::Flags flags;
  flags.launcher_dir = getLauncherDir();
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
  flags.launcher_dir = getLauncherDir();
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


TEST_F_TEMP_DISABLED_ON_WINDOWS(FetcherTest, AbsoluteFilePath)
{
  string fromDir = path::join(os::getcwd(), "from");
  ASSERT_SOME(os::mkdir(fromDir));
  string testPath = path::join(fromDir, "test");
  EXPECT_SOME(os::write(testPath, "data"));

  string localFile = path::join(os::getcwd(), "test");
  EXPECT_FALSE(os::exists(localFile));

  slave::Flags flags;
  flags.launcher_dir = getLauncherDir();

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


TEST_F_TEMP_DISABLED_ON_WINDOWS(FetcherTest, RelativeFilePath)
{
  string fromDir = path::join(os::getcwd(), "from");
  ASSERT_SOME(os::mkdir(fromDir));
  string testPath = path::join(fromDir, "test");
  EXPECT_SOME(os::write(testPath, "data"));

  string localFile = path::join(os::getcwd(), "test");
  EXPECT_FALSE(os::exists(localFile));

  slave::Flags flags;
  flags.launcher_dir = getLauncherDir();

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
  HttpProcess() {}

  MOCK_METHOD1(test, Future<http::Response>(const http::Request&));

protected:
  virtual void initialize()
  {
    route("/test", None(), &HttpProcess::test);
  }
};


class Http
{
public:
  Http() : process(new HttpProcess())
  {
    spawn(process.get());
  }

  ~Http()
  {
    terminate(process.get());
    wait(process.get());
  }

  Owned<HttpProcess> process;
};


TEST_F_TEMP_DISABLED_ON_WINDOWS(FetcherTest, OSNetUriTest)
{
  Http http;

  const network::inet::Address& address = http.process->self().address;

  process::http::URL url(
      "http",
      address.ip,
      address.port,
      path::join(http.process->self().id, "test"));

  string localFile = path::join(os::getcwd(), "test");
  EXPECT_FALSE(os::exists(localFile));

  slave::Flags flags;
  flags.launcher_dir = getLauncherDir();
  flags.frameworks_home = "/tmp/frameworks";

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  CommandInfo commandInfo;
  CommandInfo::URI* uri = commandInfo.add_uris();
  uri->set_value(stringify(url));

  Fetcher fetcher;
  SlaveID slaveId;

  EXPECT_CALL(*http.process, test(_))
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
TEST_F_TEMP_DISABLED_ON_WINDOWS(FetcherTest, OSNetUriSpaceTest)
{
  Http http;

  const network::inet::Address& address = http.process->self().address;

  process::http::URL url(
      "http",
      address.ip,
      address.port,
      path::join(http.process->self().id, "test"));

  string localFile = path::join(os::getcwd(), "test");
  EXPECT_FALSE(os::exists(localFile));

  slave::Flags flags;
  flags.launcher_dir = getLauncherDir();
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
  EXPECT_CALL(*http.process, test(_))
    .WillOnce(Return(http::OK()));

  Future<Nothing> fetch = fetcher.fetch(
      containerId, commandInfo, os::getcwd(), None(), slaveId, flags);

  AWAIT_READY(fetch);

  EXPECT_TRUE(os::exists(localFile));
}


TEST_F_TEMP_DISABLED_ON_WINDOWS(FetcherTest, FileLocalhostURI)
{
  string fromDir = path::join(os::getcwd(), "from");
  ASSERT_SOME(os::mkdir(fromDir));
  string testFile = path::join(fromDir, "test");
  EXPECT_SOME(os::write(testFile, "data"));

  string localFile = path::join(os::getcwd(), "test");
  EXPECT_FALSE(os::exists(localFile));

  slave::Flags flags;
  flags.launcher_dir = getLauncherDir();

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


TEST_F_TEMP_DISABLED_ON_WINDOWS(FetcherTest, NoExtractNotExecutable)
{
  // First construct a temporary file that can be fetched.
  Try<string> dir = os::mkdtemp(path::join(os::getcwd(), "XXXXXX"));
  ASSERT_SOME(dir);

  Try<string> path = os::mktemp(path::join(dir.get(), "XXXXXX"));
  ASSERT_SOME(path);

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  CommandInfo commandInfo;
  CommandInfo::URI* uri = commandInfo.add_uris();
  uri->set_value(path.get());
  uri->set_executable(false);
  uri->set_extract(false);

  slave::Flags flags;
  flags.launcher_dir = getLauncherDir();

  Fetcher fetcher;
  SlaveID slaveId;

  Future<Nothing> fetch = fetcher.fetch(
      containerId, commandInfo, os::getcwd(), None(), slaveId, flags);
  AWAIT_READY(fetch);

  string basename = Path(path.get()).basename();

  Try<os::Permissions> permissions = os::permissions(basename);

  ASSERT_SOME(permissions);
  EXPECT_FALSE(permissions->owner.x);
  EXPECT_FALSE(permissions->group.x);
  EXPECT_FALSE(permissions->others.x);
}


TEST_F_TEMP_DISABLED_ON_WINDOWS(FetcherTest, NoExtractExecutable)
{
  // First construct a temporary file that can be fetched.
  Try<string> dir = os::mkdtemp(path::join(os::getcwd(), "XXXXXX"));
  ASSERT_SOME(dir);

  Try<string> path = os::mktemp(path::join(dir.get(), "XXXXXX"));
  ASSERT_SOME(path);

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  CommandInfo commandInfo;
  CommandInfo::URI* uri = commandInfo.add_uris();
  uri->set_value(path.get());
  uri->set_executable(true);
  uri->set_extract(false);

  slave::Flags flags;
  flags.launcher_dir = getLauncherDir();

  Fetcher fetcher;
  SlaveID slaveId;

  Future<Nothing> fetch = fetcher.fetch(
      containerId, commandInfo, os::getcwd(), None(), slaveId, flags);

  AWAIT_READY(fetch);

  string basename = Path(path.get()).basename();

  Try<os::Permissions> permissions = os::permissions(basename);

  ASSERT_SOME(permissions);
  EXPECT_TRUE(permissions->owner.x);
  EXPECT_TRUE(permissions->group.x);
  EXPECT_TRUE(permissions->others.x);
}


TEST_F_TEMP_DISABLED_ON_WINDOWS(FetcherTest, ExtractNotExecutable)
{
  // First construct a temporary file that can be fetched and archived with tar
  // gzip.
  Try<string> dir = os::mkdtemp(path::join(os::getcwd(), "XXXXXX"));
  ASSERT_SOME(dir);

  Try<string> path = os::mktemp(path::join(dir.get(), "XXXXXX"));
  ASSERT_SOME(path);

  ASSERT_SOME(os::write(path.get(), "hello world"));

  // TODO(benh): Update os::tar so that we can capture or ignore
  // stdout/stderr output.

  // Create an uncompressed archive (see MESOS-3579), but with
  // extension `.tar.gz` to verify we can unpack files such names.
  ASSERT_SOME(os::shell(
      "tar cf '" + path.get() + ".tar.gz' '" + path.get() + "' 2>&1"));

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  CommandInfo commandInfo;
  CommandInfo::URI* uri = commandInfo.add_uris();
  uri->set_value(path.get() + ".tar.gz");
  uri->set_executable(false);
  uri->set_extract(true);

  slave::Flags flags;
  flags.launcher_dir = getLauncherDir();

  Fetcher fetcher;
  SlaveID slaveId;

  Future<Nothing> fetch = fetcher.fetch(
      containerId, commandInfo, os::getcwd(), None(), slaveId, flags);

  AWAIT_READY(fetch);

  ASSERT_TRUE(os::exists(path::join(os::getcwd(), path.get())));

  ASSERT_SOME_EQ("hello world", os::read(path::join(os::getcwd(), path.get())));

  Try<os::Permissions> permissions = os::permissions(path.get());

  ASSERT_SOME(permissions);
  EXPECT_FALSE(permissions->owner.x);
  EXPECT_FALSE(permissions->group.x);
  EXPECT_FALSE(permissions->others.x);
}


// Tests extracting tar file with extension .tar.
TEST_F_TEMP_DISABLED_ON_WINDOWS(FetcherTest, ExtractTar)
{
  // First construct a temporary file that can be fetched and archived with
  // tar.
  Try<string> dir = os::mkdtemp(path::join(os::getcwd(), "XXXXXX"));
  ASSERT_SOME(dir);

  Try<string> path = os::mktemp(path::join(dir.get(), "XXXXXX"));
  ASSERT_SOME(path);

  ASSERT_SOME(os::write(path.get(), "hello tar"));

  // TODO(benh): Update os::tar so that we can capture or ignore
  // stdout/stderr output.

  // Create an uncompressed archive (see MESOS-3579).
  ASSERT_SOME(os::shell(
      "tar cf '" + path.get() + ".tar' '" + path.get() + "' 2>&1"));

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  CommandInfo commandInfo;
  CommandInfo::URI* uri = commandInfo.add_uris();
  uri->set_value(path.get() + ".tar");
  uri->set_extract(true);

  slave::Flags flags;
  flags.launcher_dir = getLauncherDir();

  Fetcher fetcher;
  SlaveID slaveId;

  Future<Nothing> fetch = fetcher.fetch(
      containerId, commandInfo, os::getcwd(), None(), slaveId, flags);

  AWAIT_READY(fetch);

  ASSERT_TRUE(os::exists(path::join(os::getcwd(), path.get())));

  ASSERT_SOME_EQ("hello tar", os::read(path::join(os::getcwd(), path.get())));
}


TEST_F_TEMP_DISABLED_ON_WINDOWS(FetcherTest, ExtractGzipFile)
{
  // First construct a temporary file that can be fetched and archived with
  // gzip.
  Try<string> dir = os::mkdtemp(path::join(os::getcwd(), "XXXXXX"));
  ASSERT_SOME(dir);

  Try<string> path = os::mktemp(path::join(dir.get(), "XXXXXX"));
  ASSERT_SOME(path);

  ASSERT_SOME(os::write(path.get(), "hello world"));
  ASSERT_SOME(os::shell("gzip " + path.get()));

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  CommandInfo commandInfo;
  CommandInfo::URI* uri = commandInfo.add_uris();
  uri->set_value(path.get() + ".gz");
  uri->set_extract(true);

  slave::Flags flags;
  flags.launcher_dir = getLauncherDir();

  Fetcher fetcher;
  SlaveID slaveId;

  Future<Nothing> fetch = fetcher.fetch(
      containerId, commandInfo, os::getcwd(), None(), slaveId, flags);

  AWAIT_READY(fetch);

  string extractedFile = path::join(os::getcwd(), Path(path.get()).basename());
  ASSERT_TRUE(os::exists(extractedFile));

  ASSERT_SOME_EQ("hello world", os::read(extractedFile));
}


TEST_F_TEMP_DISABLED_ON_WINDOWS(FetcherTest, UNZIP_ExtractFile)
{
  // Construct a tmp file that can be fetched and archived with zip.
  string fromDir = path::join(os::getcwd(), "from");
  ASSERT_SOME(os::mkdir(fromDir));

  Try<string> path = os::mktemp(path::join(fromDir, "XXXXXX"));
  ASSERT_SOME(path);

  //  Length     Size  Cmpr    Date    Time   CRC-32   Name    Content
  // --------  ------- ---- ---------- ----- --------  ----    ------
  //       12       12   0% 2016-03-19 10:08 af083b2d  hello   hello world\n
  // --------  -------  ---                            ------- ------
  //       12       12   0%                            1 file
  ASSERT_SOME(os::write(path.get(), base64::decode(
      "UEsDBAoAAAAAABBRc0gtOwivDAAAAAwAAAAFABwAaGVsbG9VVAkAAxAX7VYQ"
      "F+1WdXgLAAEE6AMAAARkAAAAaGVsbG8gd29ybGQKUEsBAh4DCgAAAAAAEFFz"
      "SC07CK8MAAAADAAAAAUAGAAAAAAAAQAAAKSBAAAAAGhlbGxvVVQFAAMQF+1W"
      "dXgLAAEE6AMAAARkAAAAUEsFBgAAAAABAAEASwAAAEsAAAAAAA==").get()));

  ASSERT_SOME(os::rename(path.get(), path.get() + ".zip"));

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  CommandInfo commandInfo;
  CommandInfo::URI* uri = commandInfo.add_uris();
  uri->set_value(path.get() + ".zip");
  uri->set_extract(true);

  slave::Flags flags;
  flags.launcher_dir = getLauncherDir();

  Fetcher fetcher;
  SlaveID slaveId;

  Future<Nothing> fetch = fetcher.fetch(
      containerId,
      commandInfo,
      os::getcwd(),
      None(),
      slaveId,
      flags);

  AWAIT_READY(fetch);

  string extractedFile = path::join(os::getcwd(), "hello");
  ASSERT_TRUE(os::exists(extractedFile));

  ASSERT_SOME_EQ("hello world\n", os::read(extractedFile));
}


TEST_F_TEMP_DISABLED_ON_WINDOWS(FetcherTest, UNZIP_ExtractInvalidFile)
{
  // Construct a tmp file that can be filled with broken zip.
  string fromDir = path::join(os::getcwd(), "from");
  ASSERT_SOME(os::mkdir(fromDir));

  Try<string> path = os::mktemp(path::join(fromDir, "XXXXXX"));
  ASSERT_SOME(path);

  // Write broken zip to file [bad CRC 440a6aa5  (should be af083b2d)].
  //  Length     Date    Time  CRC expected  CRC actual  Name    Content
  // -------- ---------- ----- ------------  ----------  ----    ------
  //       12 2016-03-19 10:08  af083b2d     440a6aa5    world   hello hello\n
  // --------                                            ------- ------
  //       12                                            1 file
  ASSERT_SOME(os::write(path.get(), base64::decode(
      "UEsDBAoAAAAAABBRc0gtOwivDAAAAAwAAAAFABwAd29ybG9VVAkAAxAX7VYQ"
      "F+1WdXgLAAEE6AMAAARkAAAAaGVsbG8gaGVsbG8KUEsBAh4DCgAAAAAAEFFz"
      "SC07CK8MAAAADAAAAAUAGAAAAAAAAQAAAKSBAAAAAHdvcmxkVVQFAAMQF+1W"
      "dXgLAAEE6AMAAARkAAAAUEsFBgAAAAABAAEASwAAAEsAAAAAAA==").get()));

  ASSERT_SOME(os::rename(path.get(), path.get() + ".zip"));

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  CommandInfo commandInfo;
  CommandInfo::URI* uri = commandInfo.add_uris();
  uri->set_value(path.get() + ".zip");
  uri->set_extract(true);

  slave::Flags flags;
  flags.launcher_dir = getLauncherDir();

  Fetcher fetcher;
  SlaveID slaveId;

  Future<Nothing> fetch = fetcher.fetch(
      containerId,
      commandInfo,
      os::getcwd(),
      None(),
      slaveId,
      flags);

  AWAIT_FAILED(fetch);

  string extractedFile = path::join(os::getcwd(), "world");
  ASSERT_TRUE(os::exists(extractedFile));

  ASSERT_SOME_EQ("hello hello\n", os::read(extractedFile));
}


TEST_F_TEMP_DISABLED_ON_WINDOWS(
    FetcherTest,
    UNZIP_ExtractFileWithDuplicatedEntries)
{
  // Construct a tmp file that can be filled with zip containing
  // duplicates.
  string fromDir = path::join(os::getcwd(), "from");
  ASSERT_SOME(os::mkdir(fromDir));

  Try<string> path = os::mktemp(path::join(fromDir, "XXXXXX"));
  ASSERT_SOME(path);

  // Create zip file with duplicates.
  //   Length  Method    Size  Cmpr    Date    Time   CRC-32   Name   Content
  // --------  ------  ------- ---- ---------- ----- --------  ----   -------
  //       1   Stored        1   0% 2016-03-18 22:49 83dcefb7  A          1
  //       1   Stored        1   0% 2016-03-18 22:49 1ad5be0d  A          2
  // --------          -------  ---                           ------- -------
  //       2                2   0%                            2 files
  ASSERT_SOME(os::write(path.get(), base64::decode(
      "UEsDBBQAAAAAADC2cki379yDAQAAAAEAAAABAAAAQTFQSwMEFAAAAAAAMrZy"
      "SA2+1RoBAAAAAQAAAAEAAABBMlBLAQIUAxQAAAAAADC2cki379yDAQAAAAEA"
      "AAABAAAAAAAAAAAAAACAAQAAAABBUEsBAhQDFAAAAAAAMrZySA2+1RoBAAAA"
      "AQAAAAEAAAAAAAAAAAAAAIABIAAAAEFQSwUGAAAAAAIAAgBeAAAAQAAAAAAA").get()));

  ASSERT_SOME(os::rename(path.get(), path.get() + ".zip"));

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  CommandInfo commandInfo;
  CommandInfo::URI* uri = commandInfo.add_uris();
  uri->set_value(path.get() + ".zip");
  uri->set_extract(true);

  slave::Flags flags;
  flags.launcher_dir = getLauncherDir();

  Fetcher fetcher;
  SlaveID slaveId;

  Future<Nothing> fetch = fetcher.fetch(
      containerId,
      commandInfo,
      os::getcwd(),
      None(),
      slaveId,
      flags);

  AWAIT_READY(fetch);

  string extractedFile = path::join(os::getcwd(), "A");
  ASSERT_TRUE(os::exists(extractedFile));

  ASSERT_SOME_EQ("2", os::read(extractedFile));
}


TEST_F_TEMP_DISABLED_ON_WINDOWS(FetcherTest, UseCustomOutputFile)
{
  // First construct a temporary file that can be fetched.
  Try<string> dir = os::mkdtemp(path::join(os::getcwd(), "XXXXXX"));
  ASSERT_SOME(dir);

  Try<string> path = os::mktemp(path::join(dir.get(), "XXXXXX"));
  ASSERT_SOME(path);

  ASSERT_SOME(os::write(path.get(), "hello renamed file"));

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  const string customOutputFile = "custom.txt";
  CommandInfo commandInfo;
  CommandInfo::URI* uri = commandInfo.add_uris();
  uri->set_value(path.get());
  uri->set_extract(true);
  uri->set_output_file(customOutputFile);

  slave::Flags flags;
  flags.launcher_dir = getLauncherDir();

  Fetcher fetcher;
  SlaveID slaveId;

  Future<Nothing> fetch = fetcher.fetch(
      containerId, commandInfo, os::getcwd(), None(), slaveId, flags);

  AWAIT_READY(fetch);

  ASSERT_TRUE(os::exists(path::join(".", customOutputFile)));

  ASSERT_SOME_EQ(
      "hello renamed file", os::read(path::join(".", customOutputFile)));
}


TEST_F_TEMP_DISABLED_ON_WINDOWS(FetcherTest, CustomGzipOutputFile)
{
  // First construct a temporary file that can be fetched.
  Try<string> dir = os::mkdtemp(path::join(os::getcwd(), "XXXXXX"));
  ASSERT_SOME(dir);

  Try<string> path = os::mktemp(path::join(dir.get(), "XXXXXX"));
  ASSERT_SOME(path);

  ASSERT_SOME(os::write(path.get(), "hello renamed gzip file"));
  ASSERT_SOME(os::shell("gzip " + path.get()));

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  const string customOutputFile = "custom";
  CommandInfo commandInfo;
  CommandInfo::URI* uri = commandInfo.add_uris();
  uri->set_value(path.get() + ".gz");
  uri->set_extract(true);
  uri->set_output_file(customOutputFile + ".gz");

  slave::Flags flags;
  flags.launcher_dir = getLauncherDir();

  Fetcher fetcher;
  SlaveID slaveId;

  Future<Nothing> fetch = fetcher.fetch(
      containerId, commandInfo, os::getcwd(), None(), slaveId, flags);

  AWAIT_READY(fetch);

  string extractFile = path::join(".", customOutputFile);
  ASSERT_TRUE(os::exists(extractFile));

  ASSERT_SOME_EQ("hello renamed gzip file", os::read(extractFile));
}


// TODO(hausdorff): `os::chmod` does not exist on Windows.
#ifndef __WINDOWS__
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
  flags.launcher_dir = getLauncherDir();
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
#endif // __WINDOWS__


// Regression test against unwanted environment inheritance from the
// agent towards the fetcher. By supplying an invalid SSL setup, we
// force the fetcher to fail if the parent process does not filter
// them out.
TEST_F_TEMP_DISABLED_ON_WINDOWS(FetcherTest, SSLEnvironmentSpillover)
{
  // Patch some critical libprocess environment variables into the
  // parent process of the mesos-fetcher. We expect this test to fail
  // when the code path triggered does not filter them.
  char* enabled = getenv("LIBPROCESS_SSL_ENABLED");
  char* key = getenv("LIBPROCESS_SSL_KEY_FILE");

  os::setenv("LIBPROCESS_SSL_ENABLED", "true");
  os::unsetenv("LIBPROCESS_SSL_KEY_FILE");

  // First construct a temporary file that can be fetched and archived with
  // gzip.
  Try<string> dir = os::mkdtemp(path::join(os::getcwd(), "XXXXXX"));
  ASSERT_SOME(dir);

  Try<string> path = os::mktemp(path::join(dir.get(), "XXXXXX"));
  ASSERT_SOME(path);

  ASSERT_SOME(os::write(path.get(), "hello world"));
  ASSERT_SOME(os::shell("gzip " + path.get()));

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  CommandInfo commandInfo;
  CommandInfo::URI* uri = commandInfo.add_uris();
  uri->set_value(path.get() + ".gz");
  uri->set_extract(true);

  slave::Flags flags;
  flags.launcher_dir = getLauncherDir();

  Fetcher fetcher;
  SlaveID slaveId;

  Future<Nothing> fetch = fetcher.fetch(
      containerId, commandInfo, os::getcwd(), None(), slaveId, flags);

  // The mesos-fetcher runnable will fail initializing libprocess if
  // the SSL environment spilled over. Such failure would cause it to
  // abort and exit and that in turn would fail the `fetch` returned
  // future.
  AWAIT_READY(fetch);

  if (enabled != nullptr) {
    os::setenv("LIBPROCESS_SSL_ENABLED", enabled);
  } else {
    os::unsetenv("LIBPROCESS_SSL_ENABLED");
  }

  if (key != nullptr) {
    os::setenv("LIBPROCESS_SSL_KEY_FILE", key);
  }
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
