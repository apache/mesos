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

#include <stout/ip.hpp>

#include "slave/containerizer/fetcher.hpp"
#include "slave/containerizer/mesos/containerizer.hpp"
#include "slave/containerizer/mesos/isolators/network/cni/paths.hpp"

#include "tests/mesos.hpp"

namespace master = mesos::internal::master;
namespace paths = mesos::internal::slave::cni::paths;
namespace slave = mesos::internal::slave;

using master::Master;

using mesos::internal::slave::Fetcher;
using mesos::internal::slave::MesosContainerizer;

using mesos::master::detector::MasterDetector;

using process::Future;
using process::Owned;

using slave::Slave;

using std::set;
using std::string;
using std::vector;

using testing::AtMost;

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

    // TODO(jieyu): Verify that Mesos metadata is set properly.
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

  // NOTE: We use a non-shell command here because 'sh' might not be
  // in the PATH. 'alpine' does not specify env PATH in the image. On
  // some linux distribution, '/sbin' is not in the PATH by default.
  CommandInfo command;
  command.set_shell(false);
  command.set_value("/sbin/ifconfig");
  command.add_arguments("ifconfig");

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


// This test launches a long running task and checks if the CNI related
// information is checkpointed successfully once the task has been
// successfully launched. It then kills the task and checks if the
// checkpointed information is cleaned up successfully.
TEST_F(CniIsolatorTest, ROOT_VerifyCheckpointedInfo)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "network/cni";

  flags.network_cni_plugins_dir = cniPluginDir;
  flags.network_cni_config_dir = cniConfigDir;

  Fetcher fetcher;

  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, true, &fetcher);

  ASSERT_SOME(_containerizer);
  Owned<MesosContainerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), containerizer.get(), flags);
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
  command.set_value("sleep 1000");

  TaskInfo task = createTask(
      offer.slave_id(),
      Resources::parse("cpus:1;mem:128").get(),
      command);

  ContainerInfo* container = task.mutable_container();
  container->set_type(ContainerInfo::MESOS);

  // Make sure the container join the mock CNI network.
  container->add_network_infos()->set_name("__MESOS_TEST__");

  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning));

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(statusRunning);
  EXPECT_EQ(task.task_id(), statusRunning->task_id());
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  Future<hashset<ContainerID>> containers = containerizer.get()->containers();
  AWAIT_READY(containers);
  ASSERT_EQ(1u, containers.get().size());

  ContainerID containerId = *(containers.get().begin());

  // Check if the CNI related information is checkpointed successfully.
  const string containerDir =
    paths::getContainerDir(paths::ROOT_DIR, containerId.value());

  EXPECT_TRUE(os::exists(containerDir));
  EXPECT_TRUE(os::exists(paths::getNetworkDir(
      paths::ROOT_DIR, containerId.value(), "__MESOS_TEST__")));

  EXPECT_TRUE(os::exists(paths::getNetworkConfigPath(
      paths::ROOT_DIR, containerId.value(), "__MESOS_TEST__")));

  EXPECT_TRUE(os::exists(paths::getInterfaceDir(
      paths::ROOT_DIR, containerId.value(), "__MESOS_TEST__", "eth0")));

  EXPECT_TRUE(os::exists(paths::getNetworkInfoPath(
      paths::ROOT_DIR, containerId.value(), "__MESOS_TEST__", "eth0")));

  EXPECT_TRUE(os::exists(paths::getNamespacePath(
      paths::ROOT_DIR, containerId.value())));

  EXPECT_TRUE(os::exists(path::join(containerDir, "hostname")));
  EXPECT_TRUE(os::exists(path::join(containerDir, "hosts")));
  EXPECT_TRUE(os::exists(path::join(containerDir, "resolv.conf")));

  // Kill the task.
  Future<TaskStatus> statusKilled;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusKilled));

  // Wait for the executor to exit. We are using 'gc.schedule' as a proxy event
  // to monitor the exit of the executor.
  Future<Nothing> gcSchedule = FUTURE_DISPATCH(
      _, &slave::GarbageCollectorProcess::schedule);

  driver.killTask(task.task_id());

  AWAIT_READY(statusKilled);
  EXPECT_EQ(TASK_KILLED, statusKilled.get().state());

  AWAIT_READY(gcSchedule);

  // Check if the checkpointed information is cleaned up successfully.
  EXPECT_FALSE(os::exists(containerDir));

  driver.stop();
  driver.join();
}


// This test verifies that a failed CNI plugin
// will not allow a task to be launched.
TEST_F(CniIsolatorTest, ROOT_FailedPlugin)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "network/cni";

  flags.network_cni_plugins_dir = cniPluginDir;
  flags.network_cni_config_dir = cniConfigDir;

  Try<Nothing> write = os::write(
      path::join(cniPluginDir, "mockPlugin"),
      R"~(
      #!/bin/sh
      if [ x$CNI_COMMAND = xADD ]; then
        echo Plugin failed
        exit 1
      else
        exit 0
      fi
      )~");

  ASSERT_SOME(write);

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

  ContainerInfo* container = task.mutable_container();
  container->set_type(ContainerInfo::MESOS);
  container->add_network_infos()->set_name("__MESOS_TEST__");

  Future<TaskStatus> statusFailed;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusFailed));

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(statusFailed);
  EXPECT_EQ(task.task_id(), statusFailed->task_id());
  EXPECT_EQ(TASK_FAILED, statusFailed->state());

  driver.stop();
  driver.join();
}


// This test launches a command task which has checkpoint enabled, and
// agent is terminated when the task is running, after agent is restarted,
// kill the task and then verify we can receive TASK_KILLED for the task.
TEST_F(CniIsolatorTest, ROOT_SlaveRecovery)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "network/cni";

  flags.network_cni_plugins_dir = cniPluginDir;
  flags.network_cni_config_dir = cniConfigDir;

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  const Offer& offer = offers.get()[0];

  CommandInfo command;
  command.set_value("sleep 1000");

  TaskInfo task = createTask(
      offer.slave_id(),
      Resources::parse("cpus:1;mem:128").get(),
      command);

  ContainerInfo* container = task.mutable_container();
  container->set_type(ContainerInfo::MESOS);

  // Make sure the container join the mock CNI network.
  container->add_network_infos()->set_name("__MESOS_TEST__");

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusKilled;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusKilled));

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .Times(AtMost(1));

  Future<Nothing> ack =
    FUTURE_DISPATCH(_, &Slave::_statusUpdateAcknowledgement);

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(statusRunning);
  EXPECT_EQ(task.task_id(), statusRunning->task_id());
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  // Wait for the ACK to be checkpointed.
  AWAIT_READY(ack);

  // Stop the slave after TASK_RUNNING is received.
  slave.get()->terminate();

  // Restart the slave.
  slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  // Kill the task.
  driver.killTask(task.task_id());

  AWAIT_READY(statusKilled);
  EXPECT_EQ(task.task_id(), statusKilled->task_id());
  EXPECT_EQ(TASK_KILLED, statusKilled->state());

  driver.stop();
  driver.join();
}


// This test launches a container which has an image and joins host
// network, and then verifies that the container can access Internet.
TEST_F(CniIsolatorTest, ROOT_INTERNET_CURL_LaunchContainerInHostNetwork)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "docker/runtime,filesystem/linux";
  flags.image_providers = "docker";
  flags.docker_store_dir = path::join(sandbox.get(), "store");

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

  // NOTE: We use a non-shell command here because 'sh' might not be
  // in the PATH. 'alpine' does not specify env PATH in the image.
  CommandInfo command;
  command.set_shell(false);
  command.set_value("/bin/ping");
  command.add_arguments("/bin/ping");
  command.add_arguments("-c1");
  command.add_arguments("google.com");

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
