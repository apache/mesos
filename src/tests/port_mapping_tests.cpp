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

#include <gmock/gmock.h>

#include <iostream>
#include <string>
#include <vector>

#include <process/future.hpp>
#include <process/io.hpp>
#include <process/reap.hpp>
#include <process/subprocess.hpp>

#include <stout/bytes.hpp>
#include <stout/gtest.hpp>
#include <stout/ip.hpp>
#include <stout/json.hpp>
#include <stout/mac.hpp>
#include <stout/net.hpp>

#include "linux/fs.hpp"

#include "linux/routing/utils.hpp"

#include "linux/routing/filter/ip.hpp"

#include "linux/routing/link/link.hpp"

#include "linux/routing/queueing/ingress.hpp"

#include "master/master.hpp"

#include "slave/flags.hpp"
#include "slave/slave.hpp"

#include "slave/containerizer/isolators/network/port_mapping.hpp"

#include "slave/containerizer/fetcher.hpp"
#include "slave/containerizer/launcher.hpp"
#include "slave/containerizer/linux_launcher.hpp"

#include "slave/containerizer/mesos/containerizer.hpp"
#include "slave/containerizer/mesos/launch.hpp"

#include "tests/flags.hpp"
#include "tests/mesos.hpp"
#include "tests/utils.hpp"

using namespace mesos::internal::slave;

using namespace process;

using namespace routing;
using namespace routing::filter;
using namespace routing::queueing;

using mesos::internal::master::Master;

using mesos::slave::Isolator;

using std::list;
using std::ostringstream;
using std::set;
using std::string;
using std::vector;

using testing::_;
using testing::Eq;
using testing::Return;

namespace mesos {
namespace internal {
namespace tests {


// An old glibc might not have this symbol.
#ifndef MNT_DETACH
#define MNT_DETACH 2
#endif


static void cleanup(const string& eth0, const string& lo)
{
  // Clean up the ingress qdisc on eth0 and lo if exists.
  Try<bool> hostEth0ExistsQdisc = ingress::exists(eth0);
  ASSERT_SOME(hostEth0ExistsQdisc);

  if (hostEth0ExistsQdisc.get()) {
    ASSERT_SOME_TRUE(ingress::remove(eth0));
  }

  Try<bool> hostLoExistsQdisc = ingress::exists(lo);
  ASSERT_SOME(hostLoExistsQdisc);

  if (hostLoExistsQdisc.get()) {
    ASSERT_SOME_TRUE(ingress::remove(lo));
  }

  // Clean up all 'veth' devices if exist.
  Try<set<string> > links = net::links();
  ASSERT_SOME(links);

  foreach (const string& name, links.get()) {
    if (strings::startsWith(name, slave::PORT_MAPPING_VETH_PREFIX())) {
      ASSERT_SOME_TRUE(link::remove(name));
    }
  }

  Try<list<string> > entries = os::ls(slave::PORT_MAPPING_BIND_MOUNT_ROOT());
  ASSERT_SOME(entries);

  foreach (const string& file, entries.get()) {
    string target = path::join(slave::PORT_MAPPING_BIND_MOUNT_ROOT(), file);

    // NOTE: Here, we ignore the unmount errors because previous tests
    // may have created the file and died before mounting.
    mesos::internal::fs::unmount(target, MNT_DETACH);

    // Use best effort to remove the bind mount file, but it is okay
    // the file can't be removed at this point.
    os::rm(target);
  }
}


class PortMappingIsolatorTest : public TemporaryDirectoryTest
{
public:
  static void SetUpTestCase()
  {
    ASSERT_SOME(routing::check())
      << "-------------------------------------------------------------\n"
      << "We cannot run any PortMappingIsolatorTests because your\n"
      << "libnl library is not new enough. You can either install a\n"
      << "new libnl library, or disable this test case\n"
      << "-------------------------------------------------------------";

    ASSERT_SOME_EQ(0, os::shell(NULL, "which nc"))
      << "-------------------------------------------------------------\n"
      << "We cannot run any PortMappingIsolatorTests because 'nc'\n"
      << "could not be found. You can either install 'nc', or disable\n"
      << "this test case\n"
      << "-------------------------------------------------------------";

    ASSERT_SOME_EQ(0, os::shell(NULL, "which arping"))
      << "-------------------------------------------------------------\n"
      << "We cannot run some PortMappingIsolatorTests because 'arping'\n"
      << "could not be found. You can either isntall 'arping', or\n"
      << "disable this test case\n"
      << "-------------------------------------------------------------";
  }

protected:
  virtual void SetUp()
  {
    TemporaryDirectoryTest::SetUp();

    flags = CreateSlaveFlags();

    // Guess the name of the public interface.
    Result<string> _eth0 = link::eth0();
    ASSERT_SOME(_eth0) << "Failed to guess the name of the public interface";

    eth0 = _eth0.get();

    LOG(INFO) << "Using " << eth0 << " as the public interface";

    // Guess the name of the loopback interface.
    Result<string> _lo = link::lo();
    ASSERT_SOME(_lo) << "Failed to guess the name of the loopback interface";

    lo = _lo.get();

    LOG(INFO) << "Using " << lo << " as the loopback interface";

    // Clean up qdiscs and veth devices.
    cleanup(eth0, lo);

    // Get host IP address.
    Result<net::IPNetwork> _hostIPNetwork =
        net::IPNetwork::fromLinkDevice(eth0, AF_INET);

    CHECK_SOME(_hostIPNetwork)
      << "Failed to retrieve the host public IP network from " << eth0 << ": "
      << _hostIPNetwork.error();

    hostIPNetwork = _hostIPNetwork.get();

    // Get all the external name servers for tests that need to talk
    // to an external host, e.g., ping, DNS.
    Try<string> read = os::read("/etc/resolv.conf");
    CHECK_SOME(read);

    foreach (const string& line, strings::split(read.get(), "\n")) {
      if (!strings::startsWith(line, "nameserver")) {
        continue;
      }

      vector<string> tokens = strings::split(line, " ");
      ASSERT_EQ(2u, tokens.size()) << "Unexpected format in '/etc/resolv.conf'";
      if (tokens[1] != "127.0.0.1") {
        nameServers.push_back(tokens[1]);
      }
    }

    container1Ports = "ports:[31000-31499]";
    container2Ports = "ports:[31500-32000]";

    port = 31001;

    // 'errorPort' is not in 'ports' or 'ephemeral_ports'.
    errorPort = 32502;

    container1Ready = path::join(os::getcwd(), "container1_ready");
    container2Ready = path::join(os::getcwd(), "container2_ready");
    trafficViaLoopback = path::join(os::getcwd(), "traffic_via_loopback");
    trafficViaPublic = path::join(os::getcwd(), "traffic_via_public");
    exitStatus = path::join(os::getcwd(), "exit_status");
  }

  virtual void TearDown()
  {
    cleanup(eth0, lo);
    TemporaryDirectoryTest::TearDown();
  }

  slave::Flags CreateSlaveFlags()
  {
    slave::Flags flags;

    flags.launcher_dir = path::join(tests::flags.build_dir, "src");

    // NOTE: By default, Linux sets host ip local port range to
    // [32768, 61000]. We set 'ephemeral_ports' resource so that it
    // does not overlap with the host ip local port range.
    flags.resources =
      "cpus:2;mem:1024;disk:1024;ports:[31000-32000];"
      "ephemeral_ports:[30001-30999]";

    // NOTE: '16' should be enough for all our tests.
    flags.ephemeral_ports_per_container = 16;

    flags.isolation = "network/port_mapping";

    return flags;
  }

  Try<pid_t> launchHelper(
      Launcher* launcher,
      int pipes[2],
      const ContainerID& containerId,
      const string& command,
      const Option<CommandInfo>& preparation)
  {
    CommandInfo commandInfo;
    commandInfo.set_value(command);

    // The flags to pass to the helper process.
    MesosContainerizerLaunch::Flags launchFlags;

    launchFlags.command = JSON::Protobuf(commandInfo);
    launchFlags.directory = os::getcwd();

    CHECK_SOME(os::user());
    launchFlags.user = os::user().get();

    launchFlags.pipe_read = pipes[0];
    launchFlags.pipe_write = pipes[1];

    JSON::Object commands;
    JSON::Array array;
    array.values.push_back(JSON::Protobuf(preparation.get()));
    commands.values["commands"] = array;

    launchFlags.commands = commands;

    vector<string> argv(2);
    argv[0] = "mesos-containerizer";
    argv[1] = MesosContainerizerLaunch::NAME;

    Try<pid_t> pid = launcher->fork(
        containerId,
        path::join(flags.launcher_dir, "mesos-containerizer"),
        argv,
        Subprocess::FD(STDIN_FILENO),
        Subprocess::FD(STDOUT_FILENO),
        Subprocess::FD(STDERR_FILENO),
        launchFlags,
        None(),
        None());

    return pid;
  }


  JSON::Object statisticsHelper(
      pid_t pid,
      bool enable_summary,
      bool enable_details)
  {
    // Retrieve the socket information from inside the container.
    PortMappingStatistics statistics;
    statistics.flags.pid = pid;
    statistics.flags.enable_socket_statistics_summary = enable_summary;
    statistics.flags.enable_socket_statistics_details = enable_details;

    vector<string> argv(2);
    argv[0] = "mesos-network-helper";
    argv[1] = PortMappingStatistics::NAME;

    // We don't need STDIN; we need STDOUT for the result; we leave
    // STDERR as is to log to slave process.
    Try<Subprocess> s = subprocess(
        path::join(flags.launcher_dir, "mesos-network-helper"),
        argv,
        Subprocess::PATH("/dev/null"),
        Subprocess::PIPE(),
        Subprocess::FD(STDERR_FILENO),
        statistics.flags);

    CHECK_SOME(s);

    Future<Option<int> > status = s.get().status();
    AWAIT_EXPECT_READY(status);
    EXPECT_SOME_EQ(0, status.get());

    Future<string> out = io::read(s.get().out().get());
    AWAIT_EXPECT_READY(out);

    Try<JSON::Object> object = JSON::parse<JSON::Object>(out.get());
    CHECK_SOME(object);

    return object.get();
  }

  slave::Flags flags;

  // Name of the host eth0 and lo.
  string eth0;
  string lo;

  // Host public IP network.
  Option<net::IPNetwork> hostIPNetwork;

  // 'port' is within the range of ports assigned to one container.
  int port;

  // 'errorPort' is outside the range of ports assigned to the
  // container. Connecting to a container using this port will fail.
  int errorPort;

  // Ports assigned to container1.
  string container1Ports;

  // Ports assigned to container2.
  string container2Ports;

  // All the external name servers as read from /etc/resolv.conf.
  vector<string> nameServers;

  // Some auxiliary files for the tests.
  string container1Ready;
  string container2Ready;
  string trafficViaLoopback;
  string trafficViaPublic;
  string exitStatus;
};


// This test uses 2 containers: one listens to 'port' and 'errorPort'
// and writes data received to files; the other container attemptes to
// connect to the previous container using 'port' and
// 'errorPort'. Verify that only the connection through 'port' is
// successful.
TEST_F(PortMappingIsolatorTest, ROOT_ContainerToContainerTCPTest)
{
  Try<Isolator*> isolator = PortMappingIsolatorProcess::create(flags);
  CHECK_SOME(isolator);

  Try<Launcher*> launcher = LinuxLauncher::create(flags);
  CHECK_SOME(launcher);

  // Set the executor's resources.
  ExecutorInfo executorInfo;
  executorInfo.mutable_resources()->CopyFrom(
      Resources::parse(container1Ports).get());

  ContainerID containerId1;
  containerId1.set_value("container1");

  // Use a relative temporary directory so it gets cleaned up
  // automatically with the test.
  Try<string> dir1 = os::mkdtemp(path::join(os::getcwd(), "XXXXXX"));
  ASSERT_SOME(dir1);

  Future<Option<CommandInfo> > preparation1 =
    isolator.get()->prepare(containerId1, executorInfo, dir1.get(), None());

  AWAIT_READY(preparation1);
  ASSERT_SOME(preparation1.get());

  ostringstream command1;

  // Listen to 'localhost' and 'port'.
  command1 << "nc -l localhost " << port << " > " << trafficViaLoopback << "& ";

  // Listen to 'public ip' and 'port'.
  command1 << "nc -l " << hostIPNetwork.get().address() << " " << port
           << " > " << trafficViaPublic << "& ";

  // Listen to 'errorPort'. This should not get anything.
  command1 << "nc -l " << errorPort << " | tee " << trafficViaLoopback << " "
           << trafficViaPublic << "& ";

  // Touch the guard file.
  command1 << "touch " << container1Ready;

  int pipes[2];
  ASSERT_NE(-1, ::pipe(pipes));

  Try<pid_t> pid = launchHelper(
      launcher.get(),
      pipes,
      containerId1,
      command1.str(),
      preparation1.get());
  ASSERT_SOME(pid);

  // Reap the forked child.
  Future<Option<int> > status1 = process::reap(pid.get());

  // Continue in the parent.
  ::close(pipes[0]);

  // Isolate the forked child.
  AWAIT_READY(isolator.get()->isolate(containerId1, pid.get()));

  // Now signal the child to continue.
  char dummy;
  ASSERT_LT(0, ::write(pipes[1], &dummy, sizeof(dummy)));
  ::close(pipes[1]);

  // Wait for the command to start.
  while (!os::exists(container1Ready));

  ContainerID containerId2;
  containerId2.set_value("container2");

  executorInfo.mutable_resources()->CopyFrom(
      Resources::parse(container2Ports).get());

  // Use a relative temporary directory so it gets cleaned up
  // automatically with the test.
  Try<string> dir2 = os::mkdtemp(path::join(os::getcwd(), "XXXXXX"));
  ASSERT_SOME(dir2);

  Future<Option<CommandInfo> > preparation2 =
    isolator.get()->prepare(containerId2, executorInfo, dir2.get(), None());

  AWAIT_READY(preparation2);
  ASSERT_SOME(preparation2.get());

  ostringstream command2;

  // Send to 'localhost' and 'port'.
  command2 << "echo -n hello1 | nc localhost " << port << ";";
  // Send to 'localhost' and 'errorPort'. This should fail.
  command2 << "echo -n hello2 | nc localhost " << errorPort << ";";
  // Send to 'public IP' and 'port'.
  command2 << "echo -n hello3 | nc " << hostIPNetwork.get().address()
           << " " << port << ";";
  // Send to 'public IP' and 'errorPort'. This should fail.
  command2 << "echo -n hello4 | nc " << hostIPNetwork.get().address()
           << " " << errorPort << ";";
  // Touch the guard file.
  command2 << "touch " << container2Ready;

  ASSERT_NE(-1, ::pipe(pipes));

  pid = launchHelper(
      launcher.get(),
      pipes,
      containerId2,
      command2.str(),
      preparation2.get());
  ASSERT_SOME(pid);

  // Reap the forked child.
  Future<Option<int> > status2 = process::reap(pid.get());

  // Continue in the parent.
  ::close(pipes[0]);

  // Isolate the forked child.
  AWAIT_READY(isolator.get()->isolate(containerId2, pid.get()));

  // Now signal the child to continue.
  ASSERT_LT(0, ::write(pipes[1], &dummy, sizeof(dummy)));
  ::close(pipes[1]);

  // Wait for the command to start.
  while (!os::exists(container2Ready));

  // Wait for the command to complete.
  AWAIT_READY(status1);
  AWAIT_READY(status2);

  EXPECT_SOME_EQ("hello1", os::read(trafficViaLoopback));
  EXPECT_SOME_EQ("hello3", os::read(trafficViaPublic));

  // Ensure all processes are killed.
  AWAIT_READY(launcher.get()->destroy(containerId1));
  AWAIT_READY(launcher.get()->destroy(containerId2));

  // Let the isolator clean up.
  AWAIT_READY(isolator.get()->cleanup(containerId1));
  AWAIT_READY(isolator.get()->cleanup(containerId2));

  delete isolator.get();
  delete launcher.get();
}


// The same container-to-container test but with UDP.
TEST_F(PortMappingIsolatorTest, ROOT_ContainerToContainerUDPTest)
{
  Try<Isolator*> isolator = PortMappingIsolatorProcess::create(flags);
  CHECK_SOME(isolator);

  Try<Launcher*> launcher = LinuxLauncher::create(flags);
  CHECK_SOME(launcher);

  // Set the executor's resources.
  ExecutorInfo executorInfo;
  executorInfo.mutable_resources()->CopyFrom(
      Resources::parse(container1Ports).get());

  ContainerID containerId1;
  containerId1.set_value("container1");

  // Use a relative temporary directory so it gets cleaned up
  // automatically with the test.
  Try<string> dir1 = os::mkdtemp(path::join(os::getcwd(), "XXXXXX"));
  ASSERT_SOME(dir1);

  Future<Option<CommandInfo> > preparation1 =
    isolator.get()->prepare(containerId1, executorInfo, dir1.get(), None());

  AWAIT_READY(preparation1);
  ASSERT_SOME(preparation1.get());

  ostringstream command1;

  // Listen to 'localhost' and 'port'.
  command1 << "nc -u -l localhost " << port << " > "
           << trafficViaLoopback << "& ";

  // Listen to 'public ip' and 'port'.
  command1 << "nc -u -l " << hostIPNetwork.get().address() << " " << port
           << " > " << trafficViaPublic << "& ";

  // Listen to 'errorPort'. This should not receive anything.
  command1 << "nc -u -l " << errorPort << " | tee " << trafficViaLoopback << " "
           << trafficViaPublic << "& ";

  // Touch the guard file.
  command1 << "touch " << container1Ready;

  int pipes[2];
  ASSERT_NE(-1, ::pipe(pipes));

  Try<pid_t> pid = launchHelper(
      launcher.get(),
      pipes,
      containerId1,
      command1.str(),
      preparation1.get());
  ASSERT_SOME(pid);

  // Reap the forked child.
  Future<Option<int> > status1 = process::reap(pid.get());

  // Continue in the parent.
  ::close(pipes[0]);

  // Isolate the forked child.
  AWAIT_READY(isolator.get()->isolate(containerId1, pid.get()));

  // Now signal the child to continue.
  char dummy;
  ASSERT_LT(0, ::write(pipes[1], &dummy, sizeof(dummy)));
  ::close(pipes[1]);

  // Wait for the command to start.
  while (!os::exists(container1Ready));

  ContainerID containerId2;
  containerId2.set_value("container2");

  executorInfo.mutable_resources()->CopyFrom(
      Resources::parse(container2Ports).get());

  // Use a relative temporary directory so it gets cleaned up
  // automatically with the test.
  Try<string> dir2 = os::mkdtemp(path::join(os::getcwd(), "XXXXXX"));
  ASSERT_SOME(dir2);

  Future<Option<CommandInfo> > preparation2 =
    isolator.get()->prepare(containerId2, executorInfo, dir2.get(), None());

  AWAIT_READY(preparation2);
  ASSERT_SOME(preparation2.get());

  ostringstream command2;

  // Send to 'localhost' and 'port'.
  command2 << "echo -n hello1 | nc -w1 -u localhost " << port << ";";
  // Send to 'localhost' and 'errorPort'. No data should be sent.
  command2 << "echo -n hello2 | nc -w1 -u localhost " << errorPort << ";";
  // Send to 'public IP' and 'port'.
  command2 << "echo -n hello3 | nc -w1 -u " << hostIPNetwork.get().address()
           << " " << port << ";";
  // Send to 'public IP' and 'errorPort'. No data should be sent.
  command2 << "echo -n hello4 | nc -w1 -u " << hostIPNetwork.get().address()
           << " " << errorPort << ";";
  // Touch the guard file.
  command2 << "touch " << container2Ready;

  ASSERT_NE(-1, ::pipe(pipes));

  pid = launchHelper(
      launcher.get(),
      pipes,
      containerId2,
      command2.str(),
      preparation2.get());

  ASSERT_SOME(pid);

  // Reap the forked child.
  Future<Option<int> > status2 = process::reap(pid.get());

  // Continue in the parent.
  ::close(pipes[0]);

  // Isolate the forked child.
  AWAIT_READY(isolator.get()->isolate(containerId2, pid.get()));

  // Now signal the child to continue.
  ASSERT_LT(0, ::write(pipes[1], &dummy, sizeof(dummy)));
  ::close(pipes[1]);

  // Wait for the command to start.
  while (!os::exists(container2Ready));

  // Wait for the command to complete.
  AWAIT_READY(status1);
  AWAIT_READY(status2);

  EXPECT_SOME_EQ("hello1", os::read(trafficViaLoopback));
  EXPECT_SOME_EQ("hello3", os::read(trafficViaPublic));

  // Ensure all processes are killed.
  AWAIT_READY(launcher.get()->destroy(containerId1));
  AWAIT_READY(launcher.get()->destroy(containerId2));

  // Let the isolator clean up.
  AWAIT_READY(isolator.get()->cleanup(containerId1));
  AWAIT_READY(isolator.get()->cleanup(containerId2));

  delete isolator.get();
  delete launcher.get();
}


// Test the scenario where a UDP server is in a container while host
// tries to establish a UDP connection.
TEST_F(PortMappingIsolatorTest, ROOT_HostToContainerUDPTest)
{
  Try<Isolator*> isolator = PortMappingIsolatorProcess::create(flags);
  CHECK_SOME(isolator);

  Try<Launcher*> launcher = LinuxLauncher::create(flags);
  CHECK_SOME(launcher);

  // Set the executor's resources.
  ExecutorInfo executorInfo;
  executorInfo.mutable_resources()->CopyFrom(
      Resources::parse(container1Ports).get());

  ContainerID containerId;
  containerId.set_value("container1");

  // Use a relative temporary directory so it gets cleaned up
  // automatically with the test.
  Try<string> dir = os::mkdtemp(path::join(os::getcwd(), "XXXXXX"));
  ASSERT_SOME(dir);

  Future<Option<CommandInfo> > preparation1 =
    isolator.get()->prepare(containerId, executorInfo, dir.get(), None());

  AWAIT_READY(preparation1);
  ASSERT_SOME(preparation1.get());

  ostringstream command1;

  // Listen to 'localhost' and 'Port'.
  command1 << "nc -u -l localhost " << port << " > "
           << trafficViaLoopback << "&";

  // Listen to 'public IP' and 'Port'.
  command1 << "nc -u -l " << hostIPNetwork.get().address() << " " << port
           << " > " << trafficViaPublic << "&";

  // Listen to 'public IP' and 'errorPort'. This should not receive anything.
  command1 << "nc -u -l " << errorPort << " | tee " << trafficViaLoopback << " "
           << trafficViaPublic << "&";

  // Touch the guard file.
  command1 << "touch " << container1Ready;

  int pipes[2];
  ASSERT_NE(-1, ::pipe(pipes));

  Try<pid_t> pid = launchHelper(
      launcher.get(),
      pipes,
      containerId,
      command1.str(),
      preparation1.get());

  ASSERT_SOME(pid);

  // Reap the forked child.
  Future<Option<int> > status = process::reap(pid.get());

  // Continue in the parent.
  ::close(pipes[0]);

  // Isolate the forked child.
  AWAIT_READY(isolator.get()->isolate(containerId, pid.get()));

  // Now signal the child to continue.
  char dummy;
  ASSERT_LT(0, ::write(pipes[1], &dummy, sizeof(dummy)));
  ::close(pipes[1]);

  // Wait for the command to start.
  while (!os::exists(container1Ready));

  // Send to 'localhost' and 'port'.
  ASSERT_SOME_EQ(0, os::shell(
      NULL,
      "echo -n hello1 | nc -w1 -u localhost %s",
      stringify(port).c_str()));

  // Send to 'localhost' and 'errorPort'. The command should return
  // successfully because UDP is stateless but no data could be sent.
  ASSERT_SOME_EQ(0, os::shell(
      NULL,
      "echo -n hello2 | nc -w1 -u localhost %s",
      stringify(errorPort).c_str()));

  // Send to 'public IP' and 'port'.
  ASSERT_SOME_EQ(0, os::shell(
      NULL,
      "echo -n hello3 | nc -w1 -u %s %s",
      stringify(hostIPNetwork.get().address()).c_str(),
      stringify(port).c_str()));

  // Send to 'public IP' and 'errorPort'. The command should return
  // successfully because UDP is stateless but no data could be sent.
  ASSERT_SOME_EQ(0, os::shell(
      NULL,
      "echo -n hello4 | nc -w1 -u %s %s",
      stringify(hostIPNetwork.get().address()).c_str(),
      stringify(errorPort).c_str()));

  EXPECT_SOME_EQ("hello1", os::read(trafficViaLoopback));
  EXPECT_SOME_EQ("hello3", os::read(trafficViaPublic));

  // Ensure all processes are killed.
  AWAIT_READY(launcher.get()->destroy(containerId));

  // Let the isolator clean up.
  AWAIT_READY(isolator.get()->cleanup(containerId));

  delete isolator.get();
  delete launcher.get();
}


// Test the scenario where a TCP server is in a container while host
// tries to establish a TCP connection.
TEST_F(PortMappingIsolatorTest, ROOT_HostToContainerTCPTest)
{
  Try<Isolator*> isolator = PortMappingIsolatorProcess::create(flags);
  CHECK_SOME(isolator);

  Try<Launcher*> launcher = LinuxLauncher::create(flags);
  CHECK_SOME(launcher);

  // Set the executor's resources.
  ExecutorInfo executorInfo;
  executorInfo.mutable_resources()->CopyFrom(
      Resources::parse(container1Ports).get());

  ContainerID containerId;
  containerId.set_value("container1");

  // Use a relative temporary directory so it gets cleaned up
  // automatically with the test.
  Try<string> dir = os::mkdtemp(path::join(os::getcwd(), "XXXXXX"));
  ASSERT_SOME(dir);

  Future<Option<CommandInfo> > preparation1 =
    isolator.get()->prepare(containerId, executorInfo, dir.get(), None());

  AWAIT_READY(preparation1);
  ASSERT_SOME(preparation1.get());

  ostringstream command1;

  // Listen to 'localhost' and 'Port'.
  command1 << "nc -l localhost " << port << " > " << trafficViaLoopback << "&";

  // Listen to 'public IP' and 'Port'.
  command1 << "nc -l " << hostIPNetwork.get().address() << " " << port
           << " > " << trafficViaPublic << "&";

  // Listen to 'public IP' and 'errorPort'. This should fail.
  command1 << "nc -l " << errorPort << " | tee " << trafficViaLoopback << " "
           << trafficViaPublic << "&";

  // Touch the guard file.
  command1 << "touch " << container1Ready;

  int pipes[2];
  ASSERT_NE(-1, ::pipe(pipes));

  Try<pid_t> pid = launchHelper(
      launcher.get(),
      pipes,
      containerId,
      command1.str(),
      preparation1.get());

  ASSERT_SOME(pid);

  // Reap the forked child.
  Future<Option<int> > status = process::reap(pid.get());

  // Continue in the parent.
  ::close(pipes[0]);

  // Isolate the forked child.
  AWAIT_READY(isolator.get()->isolate(containerId, pid.get()));

  // Now signal the child to continue.
  char dummy;
  ASSERT_LT(0, ::write(pipes[1], &dummy, sizeof(dummy)));
  ::close(pipes[1]);

  // Wait for the command to start.
  while (!os::exists(container1Ready));

  // Send to 'localhost' and 'port'.
  ASSERT_SOME_EQ(0, os::shell(
      NULL,
      "echo -n hello1 | nc localhost %s",
      stringify(port).c_str()));

  // Send to 'localhost' and 'errorPort'. This should fail because TCP
  // connection couldn't be established..
  ASSERT_SOME_EQ(256, os::shell(
      NULL,
      "echo -n hello2 | nc localhost %s",
      stringify(errorPort).c_str()));

  // Send to 'public IP' and 'port'.
  ASSERT_SOME_EQ(0, os::shell(
      NULL,
      "echo -n hello3 | nc %s %s",
      stringify(hostIPNetwork.get().address()).c_str(),
      stringify(port).c_str()));

  // Send to 'public IP' and 'errorPort'. This should fail because TCP
  // connection couldn't be established.
  ASSERT_SOME_EQ(256, os::shell(
      NULL,
      "echo -n hello4 | nc %s %s",
      stringify(hostIPNetwork.get().address()).c_str(),
      stringify(errorPort).c_str()));

  EXPECT_SOME_EQ("hello1", os::read(trafficViaLoopback));
  EXPECT_SOME_EQ("hello3", os::read(trafficViaPublic));

  // Ensure all processes are killed.
  AWAIT_READY(launcher.get()->destroy(containerId));

  // Let the isolator clean up.
  AWAIT_READY(isolator.get()->cleanup(containerId));

  delete isolator.get();
  delete launcher.get();
}


// Test the scenario where a container issues ICMP requests to
// external hosts.
TEST_F(PortMappingIsolatorTest, ROOT_ContainerICMPExternalTest)
{
  // TODO(chzhcn): Even though this is unlikely, consider a better
  // way to get external servers.
  ASSERT_FALSE(nameServers.empty())
    << "-------------------------------------------------------------\n"
    << "We cannot run some PortMappingIsolatorTests because we could\n"
    << "not find any external name servers in /etc/resolv.conf.\n"
    << "-------------------------------------------------------------";

  Try<Isolator*> isolator = PortMappingIsolatorProcess::create(flags);
  CHECK_SOME(isolator);

  Try<Launcher*> launcher = LinuxLauncher::create(flags);
  CHECK_SOME(launcher);

  // Set the executor's resources.
  ExecutorInfo executorInfo;
  executorInfo.mutable_resources()->CopyFrom(
      Resources::parse(container1Ports).get());

  ContainerID containerId;
  containerId.set_value("container1");

  // Use a relative temporary directory so it gets cleaned up
  // automatically with the test.
  Try<string> dir = os::mkdtemp(path::join(os::getcwd(), "XXXXXX"));
  ASSERT_SOME(dir);

  Future<Option<CommandInfo> > preparation1 =
    isolator.get()->prepare(containerId, executorInfo, dir.get(), None());

  AWAIT_READY(preparation1);
  ASSERT_SOME(preparation1.get());

  ostringstream command1;
  for (unsigned int i = 0; i < nameServers.size(); i++) {
    const string& IP = nameServers[i];
    command1 << "ping -c1 " << IP;
    if (i + 1 < nameServers.size()) {
      command1 << " && ";
    }
  }
  command1 << "; echo -n $? > " << exitStatus << "; sync";

  int pipes[2];
  ASSERT_NE(-1, ::pipe(pipes));

  Try<pid_t> pid = launchHelper(
      launcher.get(),
      pipes,
      containerId,
      command1.str(),
      preparation1.get());

  ASSERT_SOME(pid);

  // Reap the forked child.
  Future<Option<int> > status = process::reap(pid.get());

  // Continue in the parent.
  ::close(pipes[0]);

  // Isolate the forked child.
  AWAIT_READY(isolator.get()->isolate(containerId, pid.get()));

  // Now signal the child to continue.
  char dummy;
  ASSERT_LT(0, ::write(pipes[1], &dummy, sizeof(dummy)));
  ::close(pipes[1]);

  // Wait for the command to complete.
  AWAIT_READY(status);

  EXPECT_SOME_EQ("0", os::read(exitStatus));

  // Ensure all processes are killed.
  AWAIT_READY(launcher.get()->destroy(containerId));

  // Let the isolator clean up.
  AWAIT_READY(isolator.get()->cleanup(containerId));

  delete isolator.get();
  delete launcher.get();
}


// Test the scenario where a container issues ICMP requests to itself.
TEST_F(PortMappingIsolatorTest, ROOT_ContainerICMPInternalTest)
{
  Try<Isolator*> isolator = PortMappingIsolatorProcess::create(flags);
  CHECK_SOME(isolator);

  Try<Launcher*> launcher = LinuxLauncher::create(flags);
  CHECK_SOME(launcher);

  // Set the executor's resources.
  ExecutorInfo executorInfo;
  executorInfo.mutable_resources()->CopyFrom(
      Resources::parse(container1Ports).get());

  ContainerID containerId;
  containerId.set_value("container1");

  // Use a relative temporary directory so it gets cleaned up
  // automatically with the test.
  Try<string> dir = os::mkdtemp(path::join(os::getcwd(), "XXXXXX"));
  ASSERT_SOME(dir);

  Future<Option<CommandInfo> > preparation1 =
    isolator.get()->prepare(containerId, executorInfo, dir.get(), None());

  AWAIT_READY(preparation1);
  ASSERT_SOME(preparation1.get());

  ostringstream command1;

  command1 << "ping -c1 127.0.0.1 && ping -c1 "
           << stringify(hostIPNetwork.get().address());

  command1 << "; echo -n $? > " << exitStatus << "; sync";

  int pipes[2];
  ASSERT_NE(-1, ::pipe(pipes));

  Try<pid_t> pid = launchHelper(
      launcher.get(),
      pipes,
      containerId,
      command1.str(),
      preparation1.get());

  ASSERT_SOME(pid);

  // Reap the forked child.
  Future<Option<int> > status = process::reap(pid.get());

  // Continue in the parent.
  ::close(pipes[0]);

  // Isolate the forked child.
  AWAIT_READY(isolator.get()->isolate(containerId, pid.get()));

  // Now signal the child to continue.
  char dummy;
  ASSERT_LT(0, ::write(pipes[1], &dummy, sizeof(dummy)));
  ::close(pipes[1]);

  // Wait for the command to complete.
  AWAIT_READY(status);

  EXPECT_SOME_EQ("0", os::read(exitStatus));

  // Ensure all processes are killed.
  AWAIT_READY(launcher.get()->destroy(containerId));

  // Let the isolator clean up.
  AWAIT_READY(isolator.get()->cleanup(containerId));

  delete isolator.get();
  delete launcher.get();
}


// Test the scenario where a container issues ARP requests to
// external hosts.
TEST_F(PortMappingIsolatorTest, ROOT_ContainerARPExternalTest)
{
  // TODO(chzhcn): Even though this is unlikely, consider a better
  // way to get external servers.
  ASSERT_FALSE(nameServers.empty())
    << "-------------------------------------------------------------\n"
    << "We cannot run some PortMappingIsolatorTests because we could\n"
    << "not find any external name servers in /etc/resolv.conf.\n"
    << "-------------------------------------------------------------";

  Try<Isolator*> isolator = PortMappingIsolatorProcess::create(flags);
  CHECK_SOME(isolator);

  Try<Launcher*> launcher = LinuxLauncher::create(flags);
  CHECK_SOME(launcher);

  // Set the executor's resources.
  ExecutorInfo executorInfo;
  executorInfo.mutable_resources()->CopyFrom(
      Resources::parse(container1Ports).get());

  ContainerID containerId;
  containerId.set_value("container1");

  // Use a relative temporary directory so it gets cleaned up
  // automatically with the test.
  Try<string> dir = os::mkdtemp(path::join(os::getcwd(), "XXXXXX"));
  ASSERT_SOME(dir);

  Future<Option<CommandInfo> > preparation1 =
    isolator.get()->prepare(containerId, executorInfo, dir.get(), None());

  AWAIT_READY(preparation1);
  ASSERT_SOME(preparation1.get());

  ostringstream command1;
  for (unsigned int i = 0; i < nameServers.size(); i++) {
    const string& IP = nameServers[i];
    // Time out after 1s and terminate upon receiving the first reply.
    command1 << "arping -f -w1 " << IP << " -I " << eth0;
    if (i + 1 < nameServers.size()) {
      command1 << " && ";
    }
  }
  command1 << "; echo -n $? > " << exitStatus << "; sync";

  int pipes[2];
  ASSERT_NE(-1, ::pipe(pipes));

  Try<pid_t> pid = launchHelper(
      launcher.get(),
      pipes,
      containerId,
      command1.str(),
      preparation1.get());

  ASSERT_SOME(pid);

  // Reap the forked child.
  Future<Option<int> > status = process::reap(pid.get());

  // Continue in the parent.
  ::close(pipes[0]);

  // Isolate the forked child.
  AWAIT_READY(isolator.get()->isolate(containerId, pid.get()));

  // Now signal the child to continue.
  char dummy;
  ASSERT_LT(0, ::write(pipes[1], &dummy, sizeof(dummy)));
  ::close(pipes[1]);

  // Wait for the command to complete.
  AWAIT_READY(status);

  EXPECT_SOME_EQ("0", os::read(exitStatus));

  // Ensure all processes are killed.
  AWAIT_READY(launcher.get()->destroy(containerId));

  // Let the isolator clean up.
  AWAIT_READY(isolator.get()->cleanup(containerId));

  delete isolator.get();
  delete launcher.get();
}


// Test DNS connectivity.
TEST_F(PortMappingIsolatorTest, ROOT_DNSTest)
{
  // TODO(chzhcn): Even though this is unlikely, consider a better
  // way to get external servers.
  ASSERT_FALSE(nameServers.empty())
    << "-------------------------------------------------------------\n"
    << "We cannot run some PortMappingIsolatorTests because we could\n"
    << "not find any external name servers in /etc/resolv.conf.\n"
    << "-------------------------------------------------------------";

  Try<Isolator*> isolator = PortMappingIsolatorProcess::create(flags);
  CHECK_SOME(isolator);

  Try<Launcher*> launcher = LinuxLauncher::create(flags);
  CHECK_SOME(launcher);

  // Set the executor's resources.
  ExecutorInfo executorInfo;
  executorInfo.mutable_resources()->CopyFrom(
      Resources::parse(container1Ports).get());

  ContainerID containerId;
  containerId.set_value("container1");

  // Use a relative temporary directory so it gets cleaned up
  // automatically with the test.
  Try<string> dir = os::mkdtemp(path::join(os::getcwd(), "XXXXXX"));
  ASSERT_SOME(dir);

  Future<Option<CommandInfo> > preparation1 =
    isolator.get()->prepare(containerId, executorInfo, dir.get(), None());

  AWAIT_READY(preparation1);
  ASSERT_SOME(preparation1.get());

  ostringstream command1;
  for (unsigned int i = 0; i < nameServers.size(); i++) {
    const string& IP = nameServers[i];
    command1 << "host " << IP;
    if (i + 1 < nameServers.size()) {
      command1 << " && ";
    }
  }
  command1 << "; echo -n $? > " << exitStatus << "; sync";

  int pipes[2];
  ASSERT_NE(-1, ::pipe(pipes));

  Try<pid_t> pid = launchHelper(
      launcher.get(),
      pipes,
      containerId,
      command1.str(),
      preparation1.get());

  ASSERT_SOME(pid);

  // Reap the forked child.
  Future<Option<int> > status = process::reap(pid.get());

  // Continue in the parent.
  ::close(pipes[0]);

  // Isolate the forked child.
  AWAIT_READY(isolator.get()->isolate(containerId, pid.get()));

  // Now signal the child to continue.
  char dummy;
  ASSERT_LT(0, ::write(pipes[1], &dummy, sizeof(dummy)));
  ::close(pipes[1]);

  // Wait for the command to complete.
  AWAIT_READY(status);

  EXPECT_SOME_EQ("0", os::read(exitStatus));

  // Ensure all processes are killed.
  AWAIT_READY(launcher.get()->destroy(containerId));

  // Let the isolator clean up.
  AWAIT_READY(isolator.get()->cleanup(containerId));

  delete isolator.get();
  delete launcher.get();
}


// Test the scenario where a container has run out of ephemeral ports
// to use.
TEST_F(PortMappingIsolatorTest, ROOT_TooManyContainersTest)
{
  // Increase the ephemeral ports per container so that we dont have
  // enough ephemeral ports to launch a second container.
  flags.ephemeral_ports_per_container = 512;

  Try<Isolator*> isolator = PortMappingIsolatorProcess::create(flags);
  CHECK_SOME(isolator);

  Try<Launcher*> launcher = LinuxLauncher::create(flags);
  CHECK_SOME(launcher);

  // Set the executor's resources.
  ExecutorInfo executorInfo;
  executorInfo.mutable_resources()->CopyFrom(
      Resources::parse(container1Ports).get());

  ContainerID containerId1;
  containerId1.set_value("container1");

  // Use a relative temporary directory so it gets cleaned up
  // automatically with the test.
  Try<string> dir1 = os::mkdtemp(path::join(os::getcwd(), "XXXXXX"));
  ASSERT_SOME(dir1);

  Future<Option<CommandInfo> > preparation1 =
    isolator.get()->prepare(containerId1, executorInfo, dir1.get(), None());

  AWAIT_READY(preparation1);
  ASSERT_SOME(preparation1.get());

  ostringstream command1;
  command1 << "sleep 1000";

  int pipes[2];
  ASSERT_NE(-1, ::pipe(pipes));

  Try<pid_t> pid = launchHelper(
      launcher.get(),
      pipes,
      containerId1,
      command1.str(),
      preparation1.get());

  ASSERT_SOME(pid);

  // Reap the forked child.
  Future<Option<int> > status1 = process::reap(pid.get());

  // Continue in the parent.
  ::close(pipes[0]);

  // Isolate the forked child.
  AWAIT_READY(isolator.get()->isolate(containerId1, pid.get()));

  // Now signal the child to continue.
  char dummy;
  ASSERT_LT(0, ::write(pipes[1], &dummy, sizeof(dummy)));
  ::close(pipes[1]);

  ContainerID containerId2;
  containerId2.set_value("container2");

  executorInfo.mutable_resources()->CopyFrom(
      Resources::parse(container2Ports).get());

  // Use a relative temporary directory so it gets cleaned up
  // automatically with the test.
  Try<string> dir2 = os::mkdtemp(path::join(os::getcwd(), "XXXXXX"));
  ASSERT_SOME(dir2);

  Future<Option<CommandInfo> > preparation2 =
    isolator.get()->prepare(containerId2, executorInfo, dir2.get(), None());

  AWAIT_FAILED(preparation2);

  // Ensure all processes are killed.
  AWAIT_READY(launcher.get()->destroy(containerId1));

  // Let the isolator clean up.
  AWAIT_READY(isolator.get()->cleanup(containerId1));

  delete isolator.get();
  delete launcher.get();
}


// Test the scenario where PortMappingIsolator uses a very small
// egress rate limit.
TEST_F(PortMappingIsolatorTest, ROOT_SmallEgressLimitTest)
{
  // Note that the underlying rate limiting mechanism usually has a
  // small allowance for burst. Empirically, as least 10x of the rate
  // limit amount of data is required to make sure the burst is an
  // insignificant factor of the transmission time.

  // To-be-tested egress rate limit, in Bytes/s.
  const Bytes rate = 2000;
  // Size of the data to send, in Bytes.
  const Bytes size = 20480;

  // Use a very small egress limit.
  flags.egress_rate_limit_per_container = rate;

  Try<Isolator*> isolator = PortMappingIsolatorProcess::create(flags);
  CHECK_SOME(isolator);

  Try<Launcher*> launcher = LinuxLauncher::create(flags);
  CHECK_SOME(launcher);

  // Open a nc server on the host side. Note that 'errorPort' is in
  // neither 'ports' nor 'ephemeral_ports', which makes it a good port
  // to use on the host.
  Try<Subprocess> s = subprocess(
      "nc -l localhost " + stringify(errorPort) + " > /devnull");

  CHECK_SOME(s);

  // Set the executor's resources.
  ExecutorInfo executorInfo;
  executorInfo.mutable_resources()->CopyFrom(
      Resources::parse(container1Ports).get());

  ContainerID containerId;
  containerId.set_value("container1");

  // Use a relative temporary directory so it gets cleaned up
  // automatically with the test.
  Try<string> dir = os::mkdtemp(path::join(os::getcwd(), "XXXXXX"));
  ASSERT_SOME(dir);

  Future<Option<CommandInfo> > preparation1 =
    isolator.get()->prepare(containerId, executorInfo, dir.get(), None());

  AWAIT_READY(preparation1);
  ASSERT_SOME(preparation1.get());

  // Fill 'size' bytes of data. The actual content does not matter.
  string data(size.bytes(), 'a');

  ostringstream command1;
  const string transmissionTime = path::join(os::getcwd(), "transmission_time");

  command1 << "echo 'Sending " << size.bytes()
           << " bytes of data under egress rate limit " << rate.bytes()
           << "Bytes/s...';";

  command1 << "{ time -p echo " << data  << " | nc localhost "
           << errorPort << " ; } 2> " << transmissionTime << " && ";

  // Touch the guard file.
  command1 << "touch " << container1Ready;

  int pipes[2];
  ASSERT_NE(-1, ::pipe(pipes));

  Try<pid_t> pid = launchHelper(
      launcher.get(),
      pipes,
      containerId,
      command1.str(),
      preparation1.get());

  ASSERT_SOME(pid);

  // Reap the forked child.
  Future<Option<int> > reap = process::reap(pid.get());

  // Continue in the parent.
  ::close(pipes[0]);

  // Isolate the forked child.
  AWAIT_READY(isolator.get()->isolate(containerId, pid.get()));

  // Now signal the child to continue.
  char dummy;
  ASSERT_LT(0, ::write(pipes[1], &dummy, sizeof(dummy)));
  ::close(pipes[1]);

  // Wait for the command to finish.
  while (!os::exists(container1Ready));

  Try<string> read = os::read(transmissionTime);
  CHECK_SOME(read);

  // Get the real elapsed time from `time` output. Sample output:
  // real 12.37
  // user 0.00
  // sys 0.00
  vector<string> lines = strings::split(strings::trim(read.get()), "\n");
  ASSERT_EQ(3u, lines.size());

  vector<string> split = strings::split(lines[0], " ");
  ASSERT_EQ(2u, split.size());

  Try<float> time = numify<float>(split[1]);
  ASSERT_SOME(time);
  ASSERT_GT(time.get(), (size.bytes() / rate.bytes()));

  // Make sure the nc server exits normally.
  Future<Option<int> > status = s.get().status();
  AWAIT_READY(status);
  EXPECT_SOME_EQ(0, status.get());

  // Ensure all processes are killed.
  AWAIT_READY(launcher.get()->destroy(containerId));

  // Let the isolator clean up.
  AWAIT_READY(isolator.get()->cleanup(containerId));

  delete isolator.get();
  delete launcher.get();
}


bool HasTCPSocketsCount(const JSON::Object& object)
{
  return object.find<JSON::Number>("net_tcp_active_connections").isSome() &&
    object.find<JSON::Number>("net_tcp_time_wait_connections").isSome();
}


bool HasTCPSocketsRTT(const JSON::Object& object)
{
  Result<JSON::Number> p50 =
    object.find<JSON::Number>("net_tcp_rtt_microsecs_p50");
  Result<JSON::Number> p90 =
    object.find<JSON::Number>("net_tcp_rtt_microsecs_p90");
  Result<JSON::Number> p95 =
    object.find<JSON::Number>("net_tcp_rtt_microsecs_p95");
  Result<JSON::Number> p99 =
    object.find<JSON::Number>("net_tcp_rtt_microsecs_p99");

  // We either have all of the following metrics or we have nothing.
  if (!p50.isSome() && !p90.isSome() && !p95.isSome() && !p99.isSome()) {
    return false;
  }

  EXPECT_TRUE(p50.isSome() && p90.isSome() && p95.isSome() && p99.isSome());

  return true;
}


// Test that RTT can be returned properly from usage(). This test is
// very similar to SmallEgressLimitTest in its setup.
TEST_F(PortMappingIsolatorTest, ROOT_PortMappingStatisticsTest)
{
  // To-be-tested egress rate limit, in Bytes/s.
  const Bytes rate = 2000;
  // Size of the data to send, in Bytes.
  const Bytes size = 20480;

  // Use a very small egress limit.
  flags.egress_rate_limit_per_container = rate;
  flags.network_enable_socket_statistics_summary = true;
  flags.network_enable_socket_statistics_details = true;

  Try<Isolator*> isolator = PortMappingIsolatorProcess::create(flags);
  CHECK_SOME(isolator);

  Try<Launcher*> launcher = LinuxLauncher::create(flags);
  CHECK_SOME(launcher);

  // Open a nc server on the host side. Note that 'errorPort' is in
  // neither 'ports' nor 'ephemeral_ports', which makes it a good port
  // to use on the host. We use this host's public IP because
  // connections to the localhost IP are filtered out when retrieving
  // the RTT information inside containers.
  Try<Subprocess> s = subprocess(
      "nc -l " + stringify(hostIPNetwork.get().address()) + " " +
      stringify(errorPort) + " > /devnull");

  CHECK_SOME(s);

  // Set the executor's resources.
  ExecutorInfo executorInfo;
  executorInfo.mutable_resources()->CopyFrom(
      Resources::parse(container1Ports).get());

  ContainerID containerId;
  containerId.set_value("container1");

  // Use a relative temporary directory so it gets cleaned up
  // automatically with the test.
  Try<string> dir1 = os::mkdtemp(path::join(os::getcwd(), "XXXXXX"));
  ASSERT_SOME(dir1);

  Future<Option<CommandInfo> > preparation1 =
    isolator.get()->prepare(containerId, executorInfo, dir1.get(), None());

  AWAIT_READY(preparation1);
  ASSERT_SOME(preparation1.get());

  // Fill 'size' bytes of data. The actual content does not matter.
  string data(size.bytes(), 'a');

  ostringstream command1;
  const string transmissionTime = path::join(os::getcwd(), "transmission_time");

  command1 << "echo 'Sending " << size.bytes()
           << " bytes of data under egress rate limit " << rate.bytes()
           << "Bytes/s...';";

  command1 << "{ time -p echo " << data  << " | nc "
           << stringify(hostIPNetwork.get().address()) << " "
           << errorPort << " ; } 2> " << transmissionTime << " && ";

  // Touch the guard file.
  command1 << "touch " << container1Ready;

  int pipes[2];
  ASSERT_NE(-1, ::pipe(pipes));

  Try<pid_t> pid = launchHelper(
      launcher.get(),
      pipes,
      containerId,
      command1.str(),
      preparation1.get());

  ASSERT_SOME(pid);

  // Reap the forked child.
  Future<Option<int> > reap = process::reap(pid.get());

  // Continue in the parent.
  ::close(pipes[0]);

  // Isolate the forked child.
  AWAIT_READY(isolator.get()->isolate(containerId, pid.get()));

  // Now signal the child to continue.
  char dummy;
  ASSERT_LT(0, ::write(pipes[1], &dummy, sizeof(dummy)));
  ::close(pipes[1]);

  // Test that RTT can be returned while transmission is going. It is
  // possible that the first few statistics returned don't have a RTT
  // value because it takes a few round-trips to actually establish a
  // tcp connection and start sending data. Nevertheless, we should
  // see a meaningful result well within seconds.
  Duration waited = Duration::zero();
  do {
    os::sleep(Milliseconds(200));
    waited += Milliseconds(200);

    // Do an end-to-end test by calling `usage`.
    Future<ResourceStatistics> usage = isolator.get()->usage(containerId);
    AWAIT_READY(usage);

    if (usage.get().has_net_tcp_rtt_microsecs_p50() &&
        usage.get().has_net_tcp_active_connections()) {
      EXPECT_GT(usage.get().net_tcp_active_connections(), 0);
      break;
    }
  } while (waited < Seconds(5));
  ASSERT_LT(waited, Seconds(5));

  // While the connection is still active, try out different flag
  // combinations.
  JSON::Object object = statisticsHelper(pid.get(), true, true);
  ASSERT_TRUE(HasTCPSocketsCount(object) && HasTCPSocketsRTT(object));

  object = statisticsHelper(pid.get(), true, false);
  ASSERT_TRUE(HasTCPSocketsCount(object) && !HasTCPSocketsRTT(object));

  object = statisticsHelper(pid.get(), false, true);
  ASSERT_TRUE(!HasTCPSocketsCount(object) && HasTCPSocketsRTT(object));

  object = statisticsHelper(pid.get(), false, false);
  ASSERT_TRUE(!HasTCPSocketsCount(object) && !HasTCPSocketsRTT(object));

  // Wait for the command to finish.
  while (!os::exists(container1Ready));

  // Make sure the nc server exits normally.
  Future<Option<int> > status = s.get().status();
  AWAIT_READY(status);
  EXPECT_SOME_EQ(0, status.get());

  // Ensure all processes are killed.
  AWAIT_READY(launcher.get()->destroy(containerId));

  // Let the isolator clean up.
  AWAIT_READY(isolator.get()->cleanup(containerId));

  delete isolator.get();
  delete launcher.get();
}


class PortMappingMesosTest : public ContainerizerTest<MesosContainerizer>
{
public:
  slave::Flags CreateSlaveFlags()
  {
    slave::Flags flags =
      ContainerizerTest<MesosContainerizer>::CreateSlaveFlags();

    // Setup recovery slave flags.
    flags.recover = "reconnect";
    flags.strict = true;

    return flags;
  }

  virtual void SetUp()
  {
    ContainerizerTest<MesosContainerizer>::SetUp();

    // Guess the name of the public interface.
    Result<string> _eth0 = link::eth0();
    ASSERT_SOME(_eth0) << "Failed to guess the name of the public interface";

    eth0 = _eth0.get();

    LOG(INFO) << "Using " << eth0 << " as the public interface";

    // Guess the name of the loopback interface.
    Result<string> _lo = link::lo();
    ASSERT_SOME(_lo) << "Failed to guess the name of the loopback interface";

    lo = _lo.get();

    LOG(INFO) << "Using " << lo << " as the loopback interface";

    cleanup(eth0, lo);

    flags = CreateSlaveFlags();
  }

  virtual void TearDown()
  {
    cleanup(eth0, lo);
    ContainerizerTest<MesosContainerizer>::TearDown();
  }

  slave::Flags flags;

  Fetcher fetcher;

  // Name of the host eth0 and lo.
  string eth0;
  string lo;
};


// Test the scenario where the network isolator is asked to recover
// both types of containers: containers that were previously managed
// by network isolator, and containers that weren't.
TEST_F(PortMappingMesosTest, ROOT_RecoverMixedContainersTest)
{
  slave::Flags flagsNoNetworkIsolator = flags;

  vector<string> isolations = strings::split(flags.isolation, ",");
  ASSERT_NE(0u, isolations.size());

  vector<string>::iterator it =
    find(isolations.begin(), isolations.end(), "network/port_mapping");

  ASSERT_NE(it, isolations.end())
    << "PortMappingMesosTests could not run because network/port_mapping"
    << "is not supported on this host.";

  // Clear part of flags to disable network isolator, but keep the
  // rest of the flags the same to share the same settings for the
  // slave.
  isolations.erase(it);
  flagsNoNetworkIsolator.isolation = strings::join(",", isolations);

  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  Try<MesosContainerizer*> containerizer1 =
    MesosContainerizer::create(flagsNoNetworkIsolator, true, &fetcher);

  ASSERT_SOME(containerizer1);

  // Start the first slave without network isolator and start a task.
  Try<PID<Slave> > slave = StartSlave(
      containerizer1.get(),
      flagsNoNetworkIsolator);

  ASSERT_SOME(slave);

  MockScheduler sched1;

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo1;
  frameworkInfo1.CopyFrom(DEFAULT_FRAMEWORK_INFO);
  frameworkInfo1.set_checkpoint(true);

  MesosSchedulerDriver driver(
      &sched1, frameworkInfo1, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched1, registered(_, _, _));

  Future<vector<Offer> > offers1;
  Future<vector<Offer> > offers2;
  EXPECT_CALL(sched1, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers1))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(DeclineOffers());      // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers1);
  EXPECT_NE(0u, offers1.get().size());

  // The first task doesn't need network resources.
  Offer offer1 = offers1.get()[0];
  offer1.mutable_resources()->CopyFrom(
      Resources::parse("cpus:1;mem:512").get());

  // Start a long running task without using network isolator.
  TaskInfo task1 = createTask(offer1, "sleep 1000");
  vector<TaskInfo> tasks1;
  tasks1.push_back(task1);

  EXPECT_CALL(sched1, statusUpdate(_, _));

  Future<Nothing> _statusUpdateAcknowledgement1 =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  driver.launchTasks(offers1.get()[0].id(), tasks1);

  // Wait for the ACK to be checkpointed.
  AWAIT_READY(_statusUpdateAcknowledgement1);

  Stop(slave.get());
  delete containerizer1.get();

  Future<Nothing> _recover = FUTURE_DISPATCH(_, &Slave::_recover);

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
      FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  // Restart the slave with a new containerizer that uses network
  // isolator.
  Try<MesosContainerizer*> containerizer2 =
    MesosContainerizer::create(flags, true, &fetcher);

  ASSERT_SOME(containerizer2);

  // Start the second slave with network isolator, recover the first
  // task without network isolation and start a second task with
  // network islation.
  slave = StartSlave(containerizer2.get(), flags);
  ASSERT_SOME(slave);

  Clock::pause();

  // Make sure the new containerizer recovers the task.
  AWAIT_READY(_recover);

  Clock::settle(); // Wait for slave to schedule reregister timeout.

  Clock::advance(EXECUTOR_REREGISTER_TIMEOUT);
  Clock::resume();

  // Wait for the slave to re-register.
  AWAIT_READY(slaveReregisteredMessage);

  AWAIT_READY(offers2);
  EXPECT_NE(0u, offers2.get().size());

  Offer offer2 = offers2.get()[0];

  TaskInfo task2 = createTask(offer2, "sleep 1000");

  vector<TaskInfo> tasks2;
  tasks2.push_back(task2); // Long-running task.

  EXPECT_CALL(sched1, statusUpdate(_, _));

  Future<Nothing> _statusUpdateAcknowledgement2 =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  driver.launchTasks(offers2.get()[0].id(), tasks2);

  // Wait for the ACK to be checkpointed.
  AWAIT_READY(_statusUpdateAcknowledgement2);

  Stop(slave.get());
  delete containerizer2.get();

  Future<Nothing> _recover2 = FUTURE_DISPATCH(_, &Slave::_recover);

  Future<SlaveReregisteredMessage> slaveReregisteredMessage2 =
      FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  // Restart the slave with a new containerizer that uses network
  // isolator. This is to verify the case where one task is running
  // without network isolator and another task is running with network
  // isolator.
  Try<MesosContainerizer*> containerizer3 =
    MesosContainerizer::create(flags, true, &fetcher);

  ASSERT_SOME(containerizer3);

  slave = StartSlave(containerizer3.get(), flags);
  ASSERT_SOME(slave);

  Clock::pause();

  // Make sure the new containerizer recovers the tasks from both runs
  // previously.
  AWAIT_READY(_recover2);

  Clock::settle(); // Wait for slave to schedule reregister timeout.

  Clock::advance(EXECUTOR_REREGISTER_TIMEOUT);
  Clock::resume();

  // Wait for the slave to re-register.
  AWAIT_READY(slaveReregisteredMessage2);

  Future<hashset<ContainerID> > containers = containerizer3.get()->containers();
  AWAIT_READY(containers);
  EXPECT_EQ(2u, containers.get().size());

  foreach (ContainerID containerId, containers.get()) {
    // Do some basic checks to make sure the network isolator can
    // handle mixed types of containers correctly.
    Future<ResourceStatistics> usage = containerizer3.get()->usage(containerId);
    AWAIT_READY(usage);

    // TODO(chzhcn): write a more thorough test for update.
    Try<Resources> resources = Containerizer::resources(flags);
    ASSERT_SOME(resources);

    Future<Nothing> update =
      containerizer3.get()->update(containerId, resources.get());

    AWAIT_READY(update);
  }

  driver.stop();
  driver.join();

  Shutdown();
  delete containerizer3.get();
}


// Test that all configurations (tc filters etc) is cleaned up for an
// orphaned container using the network isolator.
TEST_F(PortMappingMesosTest, ROOT_CleanUpOrphanTest)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  Try<MesosContainerizer*> containerizer1 =
    MesosContainerizer::create(flags, true, &fetcher);

  ASSERT_SOME(containerizer1);

  Try<PID<Slave> > slave = StartSlave(containerizer1.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo;
  frameworkInfo.CopyFrom(DEFAULT_FRAMEWORK_INFO);
  frameworkInfo.set_checkpoint(true);

  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(_, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer> > offers1;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers1))
    .WillRepeatedly(DeclineOffers());      // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers1);
  EXPECT_NE(0u, offers1.get().size());

  // Start a long running task using network islator.
  TaskInfo task = createTask(offers1.get()[0], "sleep 1000");
  vector<TaskInfo> tasks;
  tasks.push_back(task);

  EXPECT_CALL(sched, statusUpdate(_, _));

  Future<Nothing> _statusUpdateAcknowledgement1 =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  driver.launchTasks(offers1.get()[0].id(), tasks);

  // Wait for the ACK to be checkpointed.
  AWAIT_READY(_statusUpdateAcknowledgement1);

  Stop(slave.get());
  delete containerizer1.get();

  ExecutorID executorId;
  executorId.set_value(task.task_id().value());

  // Construct the framework meta directory that needs wiping.
  string frameworkPath = paths::getFrameworkPath(
      paths::getMetaRootDir(flags.work_dir),
      offers1.get()[0].slave_id(),
      frameworkId.get());

  // Remove the framework meta directory, so that the slave will not
  // recover the task.
  ASSERT_SOME(os::rmdir(frameworkPath, true));

  Future<Nothing> _recover = FUTURE_DISPATCH(_, &Slave::_recover);

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return());        // Ignore subsequent updates.

  // Restart the slave (use same flags) with a new containerizer.
  Try<MesosContainerizer*> containerizer2 =
    MesosContainerizer::create(flags, true, &fetcher);

  ASSERT_SOME(containerizer2);

  slave = StartSlave(containerizer2.get(), flags);
  ASSERT_SOME(slave);

  // Wait for the slave to recover.
  AWAIT_READY(_recover);

  // Wait for the slave to re-register.
  AWAIT_READY(slaveReregisteredMessage);

  // Wait for TASK_LOST update.
  AWAIT_READY(status);
  ASSERT_EQ(TASK_LOST, status.get().state());

  // Expect that qdiscs still exist on eth0 and lo but with no filters.
  Try<bool> hostEth0ExistsQdisc = ingress::exists(eth0);
  EXPECT_SOME_TRUE(hostEth0ExistsQdisc);

  Try<bool> hostLoExistsQdisc = ingress::exists(lo);
  EXPECT_SOME_TRUE(hostLoExistsQdisc);

  Result<vector<ip::Classifier> > classifiers =
    ip::classifiers(eth0, ingress::HANDLE);

  EXPECT_SOME(classifiers);
  EXPECT_EQ(0u, classifiers.get().size());

  classifiers = ip::classifiers(lo, ingress::HANDLE);
  EXPECT_SOME(classifiers);
  EXPECT_EQ(0u, classifiers.get().size());

  // Expect no 'veth' devices.
  Try<set<string> > links = net::links();
  ASSERT_SOME(links);
  foreach (const string& name, links.get()) {
    EXPECT_FALSE(strings::startsWith(name, slave::PORT_MAPPING_VETH_PREFIX()));
  }

  // Expect no files in bind mount directory.
  Try<list<string> > files = os::ls(slave::PORT_MAPPING_BIND_MOUNT_ROOT());
  ASSERT_SOME(files);
  EXPECT_EQ(0u, files.get().size());

  driver.stop();
  driver.join();

  Shutdown();
  delete containerizer2.get();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
