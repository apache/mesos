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
#include <process/reap.hpp>

#include <stout/gtest.hpp>
#include <stout/json.hpp>
#include <stout/net.hpp>

#include "linux/routing/utils.hpp"

#include "linux/routing/filter/ip.hpp"

#include "linux/routing/link/link.hpp"

#include "linux/routing/queueing/ingress.hpp"

#include "master/master.hpp"

#include "slave/flags.hpp"
#include "slave/slave.hpp"

#include "slave/containerizer/isolators/network/port_mapping.hpp"

#include "slave/containerizer/launcher.hpp"
#include "slave/containerizer/linux_launcher.hpp"

#include "slave/containerizer/mesos/containerizer.hpp"
#include "slave/containerizer/mesos/launch.hpp"

#include "tests/flags.hpp"
#include "tests/mesos.hpp"
#include "tests/utils.hpp"

using namespace mesos;

using namespace mesos::internal;
using namespace mesos::internal::slave;
using namespace mesos::internal::tests;

using namespace process;

using namespace routing;
using namespace routing::filter;
using namespace routing::queueing;

using mesos::internal::master::Master;

using mesos::internal::slave::Launcher;
using mesos::internal::slave::LinuxLauncher;
using mesos::internal::slave::MesosContainerizer;
using mesos::internal::slave::MesosContainerizerLaunch;
using mesos::internal::slave::PortMappingIsolatorProcess;

using std::list;
using std::ostringstream;
using std::set;
using std::string;
using std::vector;

using testing::_;
using testing::Eq;
using testing::Return;

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
    if (strings::startsWith(name, slave::VETH_PREFIX)) {
      ASSERT_SOME_TRUE(link::remove(name));
    }
  }

  foreach (const string& file, os::ls(slave::BIND_MOUNT_ROOT)) {
    CHECK_SOME(os::rm(file));
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
    Result<net::IP> _hostIP = net::ip(eth0);
    CHECK_SOME(_hostIP)
      << "Failed to retrieve the host public IP from " << eth0 << ": "
      << _hostIP.error();

    hostIP = _hostIP.get();

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

    // errorPort is not in resources and private_resources.
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
    flags.resources = "cpus:2;mem:1024;disk:1024;ports:[31000-32000]";
    flags.isolation = "network/port_mapping";
    flags.private_resources = "ports:" + slave::DEFAULT_EPHEMERAL_PORTS;

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

  slave::Flags flags;

  // Name of the host eth0 and lo.
  string eth0;
  string lo;

  // Host public IP.
  Option<net::IP> hostIP;

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

  Future<Option<CommandInfo> > preparation1 =
    isolator.get()->prepare(containerId1, executorInfo);
  AWAIT_READY(preparation1);
  ASSERT_SOME(preparation1.get());

  ostringstream command1;
  // Listen to 'localhost' and 'port'.
  command1 << "nc -l localhost " << port << " > " << trafficViaLoopback << "& ";

  // Listen to 'public ip' and 'port'.
  command1 << "nc -l " << net::IP(hostIP.get().address()) << " " << port
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

  Future<Option<CommandInfo> > preparation2 =
    isolator.get()->prepare(containerId2, executorInfo);
  AWAIT_READY(preparation2);
  ASSERT_SOME(preparation2.get());

  ostringstream command2;
  // Send to 'localhost' and 'port'.
  command2 << "echo -n hello1 | nc localhost " << port << ";";
  // Send to 'localhost' and 'errorPort'. This should fail.
  command2 << "echo -n hello2 | nc localhost " << errorPort << ";";
  // Send to 'public IP' and 'port'.
  command2 << "echo -n hello3 | nc " << net::IP(hostIP.get().address())
           << " " << port << ";";
  // Send to 'public IP' and 'errorPort'. This should fail.
  command2 << "echo -n hello4 | nc " << net::IP(hostIP.get().address())
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

  Future<Option<CommandInfo> > preparation1 =
    isolator.get()->prepare(containerId1, executorInfo);
  AWAIT_READY(preparation1);
  ASSERT_SOME(preparation1.get());

  ostringstream command1;
  // Listen to 'localhost' and 'port'.
  command1 << "nc -u -l localhost " << port << " > "
           << trafficViaLoopback << "& ";

  // Listen to 'public ip' and 'port'.
  command1 << "nc -u -l " << net::IP(hostIP.get().address()) << " " << port
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

  Future<Option<CommandInfo> > preparation2 =
    isolator.get()->prepare(containerId2, executorInfo);
  AWAIT_READY(preparation2);
  ASSERT_SOME(preparation2.get());

  ostringstream command2;
  // Send to 'localhost' and 'port'.
  command2 << "echo -n hello1 | nc -w1 -u localhost " << port << ";";
  // Send to 'localhost' and 'errorPort'. No data should be sent.
  command2 << "echo -n hello2 | nc -w1 -u localhost " << errorPort << ";";
  // Send to 'public IP' and 'port'.
  command2 << "echo -n hello3 | nc -w1 -u " << net::IP(hostIP.get().address())
           << " " << port << ";";
  // Send to 'public IP' and 'errorPort'. No data should be sent.
  command2 << "echo -n hello4 | nc -w1 -u " << net::IP(hostIP.get().address())
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

  Future<Option<CommandInfo> > preparation1 =
    isolator.get()->prepare(containerId, executorInfo);
  AWAIT_READY(preparation1);
  ASSERT_SOME(preparation1.get());

  ostringstream command1;
  // Listen to 'localhost' and 'Port'.
  command1 << "nc -u -l localhost " << port << " > "
           << trafficViaLoopback << "&";

  // Listen to 'public IP' and 'Port'.
  command1 << "nc -u -l " << net::IP(hostIP.get().address()) << " " << port
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
      stringify(net::IP(hostIP.get().address())).c_str(),
      stringify(port).c_str()));

  // Send to 'public IP' and 'errorPort'. The command should return
  // successfully because UDP is stateless but no data could be sent.
  ASSERT_SOME_EQ(0, os::shell(
      NULL,
      "echo -n hello4 | nc -w1 -u %s %s",
      stringify(net::IP(hostIP.get().address())).c_str(),
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

  Future<Option<CommandInfo> > preparation1 =
    isolator.get()->prepare(containerId, executorInfo);
  AWAIT_READY(preparation1);
  ASSERT_SOME(preparation1.get());

  ostringstream command1;
  // Listen to 'localhost' and 'Port'.
  command1 << "nc -l localhost " << port << " > " << trafficViaLoopback << "&";

  // Listen to 'public IP' and 'Port'.
  command1 << "nc -l " << net::IP(hostIP.get().address()) << " " << port
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
      stringify(net::IP(hostIP.get().address())).c_str(),
      stringify(port).c_str()));

  // Send to 'public IP' and 'errorPort'. This should fail because TCP
  // connection couldn't be established.
  ASSERT_SOME_EQ(256, os::shell(
      NULL,
      "echo -n hello4 | nc %s %s",
      stringify(net::IP(hostIP.get().address())).c_str(),
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

  Future<Option<CommandInfo> > preparation1 =
    isolator.get()->prepare(containerId, executorInfo);
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

  Future<Option<CommandInfo> > preparation1 =
    isolator.get()->prepare(containerId, executorInfo);
  AWAIT_READY(preparation1);
  ASSERT_SOME(preparation1.get());

  ostringstream command1;
  command1 << "ping -c1 127.0.0.1 && ping -c1 "
           << stringify(net::IP(hostIP.get().address()));
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

  Future<Option<CommandInfo> > preparation1 =
    isolator.get()->prepare(containerId, executorInfo);
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

  Future<Option<CommandInfo> > preparation1 =
    isolator.get()->prepare(containerId, executorInfo);
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

  Future<Option<CommandInfo> > preparation1 =
    isolator.get()->prepare(containerId1, executorInfo);
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

  Future<Option<CommandInfo> > preparation2 =
    isolator.get()->prepare(containerId2, executorInfo);
  AWAIT_FAILED(preparation2);

  // Ensure all processes are killed.
  AWAIT_READY(launcher.get()->destroy(containerId1));

  // Let the isolator clean up.
  AWAIT_READY(isolator.get()->cleanup(containerId1));

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
    flags.checkpoint = true;
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
  flagsNoNetworkIsolator.private_resources = "";

  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  Try<MesosContainerizer*> containerizer1 =
    MesosContainerizer::create(flagsNoNetworkIsolator, true);
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
    MesosContainerizer::create(flags, true);
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
  tasks2.push_back(task2); // Long-running task

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
    MesosContainerizer::create(flags, true);
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
    Future<ResourceStatistics> usage =
      containerizer3.get()->usage(containerId);
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
    MesosContainerizer::create(flags, true);
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
    MesosContainerizer::create(flags, true);
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
    EXPECT_FALSE(strings::startsWith(name, slave::VETH_PREFIX));
  }

  // Expect no files in bind mount directory.
  list<string> files = os::ls(slave::BIND_MOUNT_ROOT);
  EXPECT_EQ(0u, files.size());

  driver.stop();
  driver.join();

  Shutdown();
  delete containerizer2.get();
}
