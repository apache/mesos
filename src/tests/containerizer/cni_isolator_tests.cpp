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

#include <stout/ip.hpp>

#include "tests/mesos.hpp"

using mesos::master::detector::MasterDetector;

using process::Future;
using process::Owned;

using std::set;
using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace tests {

class CniIsolatorTest : public MesosTest
{
public:
  virtual void SetUp()
  {
    MesosTest::SetUp();

    Try<set<string>> links = net::links();
    ASSERT_SOME(links);

    Result<net::IPNetwork> hostIPNetwork = None();
    foreach (const string& link, links.get()) {
      hostIPNetwork = net::IPNetwork::fromLinkDevice(link, AF_INET);
      EXPECT_FALSE(hostIPNetwork.isError());

      if (hostIPNetwork.isSome() &&
          (hostIPNetwork.get() != net::IPNetwork::LOOPBACK_V4())) {
        break;
      }
    }

    ASSERT_SOME(hostIPNetwork);

    // Get the first external name server.
    Try<string> read = os::read("/etc/resolv.conf");
    ASSERT_SOME(read);

    Option<string> nameServer;
    foreach (const string& line, strings::split(read.get(), "\n")) {
      if (!strings::startsWith(line, "nameserver")) {
        continue;
      }

      vector<string> tokens = strings::split(line, " ");
      ASSERT_LE(2u, tokens.size()) << "Unexpected format in '/etc/resolv.conf'";
      if (tokens[1] != "127.0.0.1") {
        nameServer = tokens[1];
        break;
      }
    }

    ASSERT_SOME(nameServer);

    // Generate the mock CNI plugin.
    cniPluginDir = path::join(sandbox.get(), "plugins");
    ASSERT_SOME(os::mkdir(cniPluginDir));

    Try<Nothing> write = os::write(
        path::join(cniPluginDir, "mockPlugin"),
        strings::format(R"~(
        #!/bin/sh
        echo "{"
        echo "  \"ip4\": {"
        echo "    \"ip\": \"%s/%d\""
        echo "  },"
        echo "  \"dns\": {"
        echo "    \"nameservers\": [ \"%s\" ]"
        echo "  }"
        echo "}"
        )~",
        hostIPNetwork.get().address(),
        hostIPNetwork.get().prefix(),
        nameServer.get()).get());

    ASSERT_SOME(write);

    // Make sure the plugin has execution permission.
    ASSERT_SOME(os::chmod(
        path::join(cniPluginDir, "mockPlugin"),
        S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH));

    // Generate the mock CNI config.
    cniConfigDir = path::join(sandbox.get(), "configs");
    ASSERT_SOME(os::mkdir(cniConfigDir));

    write = os::write(
        path::join(cniConfigDir, "mockConfig"),
        R"~(
        {
          "name": "__MESOS_TEST__",
          "type": "mockPlugin"
        })~");

    ASSERT_SOME(write);
  }

  string cniPluginDir;
  string cniConfigDir;
};


// This test verifies that a container is created and joins a mock CNI
// network, and a command task is executed in the container successfully.
TEST_F(CniIsolatorTest, ROOT_INTERNET_CURL_LaunchCommandTask)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "docker/runtime,filesystem/linux,network/cni";
  flags.image_providers = "docker";
  flags.docker_store_dir = path::join(sandbox.get(), "store");

  flags.network_cni_plugins_dir = cniPluginDir;
  flags.network_cni_config_dir = cniConfigDir;

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  const Offer& offer = offers.get()[0];

  CommandInfo command;
  command.set_value("ifconfig");

  TaskInfo task = createTask(
      offer.slave_id(),
      Resources::parse("cpus:1;mem:128").get(),
      command);

  Image image;
  image.set_type(Image::DOCKER);
  image.mutable_docker()->set_name("alpine");

  ContainerInfo* container = task.mutable_container();
  container->set_type(ContainerInfo::MESOS);
  container->mutable_mesos()->mutable_image()->CopyFrom(image);

  // Make sure the container join the mock CNI network.
  container->add_network_infos()->set_name("__MESOS_TEST__");

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished));

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(task.task_id(), statusRunning->task_id());
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  AWAIT_READY(statusFinished);
  EXPECT_EQ(task.task_id(), statusFinished->task_id());
  EXPECT_EQ(TASK_FINISHED, statusFinished->state());

  driver.stop();
  driver.join();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
