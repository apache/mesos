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

#include <gtest/gtest.h>

#include <process/clock.hpp>

#include "slave/containerizer/fetcher.hpp"
#include "slave/containerizer/mesos/containerizer.hpp"
#include "slave/containerizer/mesos/isolators/network/cni/paths.hpp"
#include "slave/containerizer/mesos/isolators/network/cni/spec.hpp"

#include "tests/mesos.hpp"

namespace master = mesos::internal::master;
namespace paths = mesos::internal::slave::cni::paths;
namespace slave = mesos::internal::slave;
namespace spec = mesos::internal::slave::cni::spec;

using master::Master;

using mesos::internal::slave::Fetcher;
using mesos::internal::slave::MesosContainerizer;

using mesos::internal::tests::common::createNetworkInfo;

using mesos::master::detector::MasterDetector;

using mesos::v1::scheduler::Event;

using process::Clock;
using process::Future;
using process::Owned;

using slave::Slave;

using std::ostream;
using std::set;
using std::string;
using std::vector;

using testing::AtMost;
using testing::DoAll;
using testing::WithParamInterface;

namespace mesos {
namespace internal {
namespace tests {

constexpr char MESOS_CNI_PORT_MAPPER_NETWORK[] = "__MESOS_TEST__portMapper";
constexpr char MESOS_MOCK_CNI_CONFIG[] = "mockConfig";
constexpr char MESOS_TEST_PORT_MAPPER_CHAIN[] = "MESOS-TEST-PORT-MAPPER-CHAIN";


TEST(CniSpecTest, GenerateResolverConfig)
{
  spec::DNS dns;

  EXPECT_EQ("", spec::formatResolverConfig(dns));

  dns.Clear();
  dns.set_domain("m.a.org");
  EXPECT_EQ("domain m.a.org\n", spec::formatResolverConfig(dns));

  dns.Clear();
  dns.add_nameservers("1.1.1.1");
  dns.add_nameservers("2.2.2.2");
  EXPECT_EQ(
      "nameserver 1.1.1.1\n"
      "nameserver 2.2.2.2\n",
      spec::formatResolverConfig(dns));

  dns.Clear();
  dns.add_search("a.m.a.org");
  dns.add_search("b.m.a.org");
  EXPECT_EQ(
      "search a.m.a.org b.m.a.org\n",
      spec::formatResolverConfig(dns));

  dns.Clear();
  dns.add_options("debug");
  dns.add_options("ndots:2");
  EXPECT_EQ(
      "options debug ndots:2\n",
      spec::formatResolverConfig(dns));
}


class CniIsolatorTest : public MesosTest
{
public:
  virtual void SetUp()
  {
    MesosTest::SetUp();

    cniPluginDir = path::join(sandbox.get(), "plugins");
    cniConfigDir = path::join(sandbox.get(), "configs");

    Try<net::IP::Network> hostNetwork = getNonLoopbackIP();

    ASSERT_SOME(hostNetwork);

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

    // Set up the default CNI plugin.
    Try<Nothing> result = setupMockPlugin(
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
        hostNetwork->address(),
        hostNetwork->prefix(),
        nameServer.get()).get());

    ASSERT_SOME(result);

    // Generate the mock CNI config.
    ASSERT_SOME(os::mkdir(cniConfigDir));

    result = os::write(
        path::join(cniConfigDir, MESOS_MOCK_CNI_CONFIG),
        R"~(
        {
          "name": "__MESOS_TEST__",
          "type": "mockPlugin"
        })~");

    ASSERT_SOME(result);
  }

  // Generate the mock CNI plugin based on the given script.
  Try<Nothing> setupMockPlugin(const string& pluginScript)
  {
    Try<Nothing> mkdir = os::mkdir(cniPluginDir);
    if (mkdir.isError()) {
      return Error("Failed to mkdir '" + cniPluginDir + "': " + mkdir.error());
    }

    string mockPlugin = path::join(cniPluginDir, "mockPlugin");

    Try<Nothing> write = os::write(mockPlugin, pluginScript);
    if (write.isError()) {
      return Error("Failed to write '" + mockPlugin + "': " + write.error());
    }

    // Make sure the plugin has execution permission.
    Try<Nothing> chmod = os::chmod(
        mockPlugin,
        S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);

    if (chmod.isError()) {
      return Error("Failed to chmod '" + mockPlugin + "': " + chmod.error());
    }

    return Nothing();
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

  Fetcher fetcher(flags);

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
  ASSERT_EQ(1u, containers->size());

  ContainerID containerId = *(containers->begin());

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
  EXPECT_EQ(TASK_KILLED, statusKilled->state());

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


// This test verifies that the environment variable 'LIBPROCESS_IP' is
// properly set to 0.0.0.0 (instead of the agent IP) for the container
// if it joins a non-host CNI network.
TEST_F(CniIsolatorTest, ROOT_EnvironmentLibprocessIP)
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

  MesosSchedulerDriver driver(
      &sched,
      DEFAULT_FRAMEWORK_INFO,
      master.get()->pid,
      DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  const Offer& offer = offers.get()[0];

  const string command =
      R"~(
      #!/bin/sh
      if [ x"$LIBPROCESS_IP" = x"0.0.0.0" ]; then
        exit 0
      else
        exit 1
      fi)~";

  TaskInfo task = createTask(
      offer,
      command);

  ContainerInfo* container = task.mutable_container();
  container->set_type(ContainerInfo::MESOS);

  // Make sure the container joins the mock CNI network.
  container->add_network_infos()->set_name("__MESOS_TEST__");

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished));

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(statusRunning);
  EXPECT_EQ(task.task_id(), statusRunning->task_id());
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  AWAIT_READY(statusFinished);
  EXPECT_EQ(task.task_id(), statusFinished->task_id());
  EXPECT_EQ(TASK_FINISHED, statusFinished->state());

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


// This tests the dynamic addition and deletion of CNI configuration
// without the need to restart the agent.
TEST_F(CniIsolatorTest, ROOT_DynamicAddDelofCniConfig)
{
  master::Flags masterFlags = CreateMasterFlags();

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();

  slaveFlags.network_cni_plugins_dir = cniPluginDir;
  slaveFlags.network_cni_config_dir = cniConfigDir;

  Try<string> mockCniConfig = os::read(path::join(cniConfigDir, "mockConfig"));
  ASSERT_SOME(mockCniConfig);

  // Remove the CNI config.
  Try<Nothing> rm = os::rm(path::join(cniConfigDir, "mockConfig"));
  ASSERT_SOME(rm);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  MockScheduler sched;

  // We use the filter explicitly here so that the resources will not
  // be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;

  MesosSchedulerDriver driver(
      &sched,
      frameworkInfo,
      master.get()->pid,
      DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(DeclineOffers()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  const Offer& offer1 = offers.get()[0];

  CommandInfo command = createCommandInfo("sleep 1000");

  TaskInfo task = createTask(
      offer1.slave_id(),
      Resources::parse("cpus:0.1;mem:128").get(),
      command);

  ContainerInfo* container = task.mutable_container();
  container->set_type(ContainerInfo::MESOS);

  // Make sure the container is not able to join mock CNI network.
  container->add_network_infos()->set_name("__MESOS_TEST__");

  Future<TaskStatus> statusFailed;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusFailed));

  driver.launchTasks(offer1.id(), {task}, filters);

  AWAIT_READY_FOR(statusFailed, Seconds(60));
  EXPECT_EQ(task.task_id(), statusFailed->task_id());
  EXPECT_EQ(TASK_FAILED, statusFailed->state());

  // Now add the CNI config back. This would dynamically add the CNI
  // network to the `network/cni` isolator, and try launching a task
  // on this CNI network.
  Try<Nothing> write = os::write(
      path::join(cniConfigDir, "mockConfig"),
      mockCniConfig.get());

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(DeclineOffers()); // Ignore subsequent offers.

  Clock::pause();
  Clock::advance(masterFlags.allocation_interval);
  Clock::settle();
  Clock::resume();

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  const Offer& offer2 = offers.get()[0];

  task = createTask(
      offer2.slave_id(),
      Resources::parse("cpus:0.1;mem:128").get(),
      command);

  container = task.mutable_container();
  container->set_type(ContainerInfo::MESOS);

  // Make sure the container is able to join mock CNI network.
  container->add_network_infos()->set_name("__MESOS_TEST__");

  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning));

  driver.launchTasks(offer2.id(), {task}, filters);

  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(task.task_id(), statusRunning->task_id());
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  // Testing dynamic deletion of CNI networks.
  rm = os::rm(path::join(cniConfigDir, "mockConfig"));
  ASSERT_SOME(rm);

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(DeclineOffers()); // Ignore subsequent offers.

  Clock::pause();
  Clock::advance(Seconds(10));
  Clock::settle();
  Clock::resume();

  // Try launching the task on the `__MESOS_TEST__` network, it should
  // fail because the network config has been deleted.
  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  const Offer& offer3 = offers.get()[0];

  task = createTask(
      offer3.slave_id(),
      Resources::parse("cpus:0.1;mem:128").get(),
      command);

  container = task.mutable_container();
  container->set_type(ContainerInfo::MESOS);

  // Make sure the container is not able to join mock CNI network.
  container->add_network_infos()->set_name("__MESOS_TEST__");

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusFailed));

  driver.launchTasks(offer3.id(), {task}, filters);

  AWAIT_READY_FOR(statusFailed, Seconds(60));
  EXPECT_EQ(task.task_id(), statusFailed->task_id());
  EXPECT_EQ(TASK_FAILED, statusFailed->state());

  driver.stop();
  driver.join();
}


// This test verifies that the hostname of the container can be
// overridden by setting hostname field in ContainerInfo.
TEST_F(CniIsolatorTest, ROOT_OverrideHostname)
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

  MesosSchedulerDriver driver(
      &sched,
      DEFAULT_FRAMEWORK_INFO,
      master.get()->pid,
      DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  const Offer& offer = offers.get()[0];

  const string command =
      R"~(
      #!/bin/sh
      NAME=`hostname`
      if [ x"$NAME" = x"test" ]; then
        exit 0
      else
        exit 1
      fi)~";

  TaskInfo task = createTask(
      offer,
      command);

  ContainerInfo* container = task.mutable_container();
  container->set_type(ContainerInfo::MESOS);
  container->set_hostname("test");

  // Make sure the container joins the mock CNI network.
  container->add_network_infos()->set_name("__MESOS_TEST__");

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished));

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(statusRunning);
  EXPECT_EQ(task.task_id(), statusRunning->task_id());
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  AWAIT_READY(statusFinished);
  EXPECT_EQ(task.task_id(), statusFinished->task_id());
  EXPECT_EQ(TASK_FINISHED, statusFinished->state());

  driver.stop();
  driver.join();
}


// This test checks that a CNI DNS configuration ends up generating
// the right settings in /etc/resolv.conf.
TEST_F(CniIsolatorTest, ROOT_VerifyResolverConfig)
{
  Try<net::IP::Network> hostNetwork = getNonLoopbackIP();
  ASSERT_SOME(hostNetwork);

  Try<string> mockPlugin = strings::format(
      R"~(
      #!/bin/sh
      echo '{'
      echo '  "ip4": {'
      echo '    "ip": "%s/%d"'
      echo '  },'
      echo '  "dns": {'
      echo '    "nameservers": ['
      echo '      "1.1.1.1",'
      echo '      "1.1.1.2"'
      echo '    ],'
      echo '    "domain": "mesos.apache.org",'
      echo '    "search": ['
      echo '      "a.mesos.apache.org",'
      echo '      "a.mesos.apache.org"'
      echo '    ],'
      echo '    "options":['
      echo '      "option1",'
      echo '      "option2"'
      echo '    ]'
      echo '  }'
      echo '}'
      )~",
      hostNetwork.get().address(),
      hostNetwork.get().prefix());

  ASSERT_SOME(mockPlugin);

  ASSERT_SOME(setupMockPlugin(mockPlugin.get()));

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

  MesosSchedulerDriver driver(
      &sched,
      DEFAULT_FRAMEWORK_INFO,
      master.get()->pid,
      DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  const Offer& offer = offers.get()[0];

  // Verify that /etc/resolv.conf was generated the way we expect.
  // This is sensitive to changes in 'formatResolverConfig()'.
  const string command =
      "#! /bin/sh\n"
      "set -x\n"
      "cat > expected <<EOF\n"
      "domain mesos.apache.org\n"
      "search a.mesos.apache.org a.mesos.apache.org\n"
      "options option1 option2\n"
      "nameserver 1.1.1.1\n"
      "nameserver 1.1.1.2\n"
      "EOF\n"
      "cat /etc/resolv.conf\n"
      "exec diff -c /etc/resolv.conf expected\n";

  TaskInfo task = createTask(offer, command);

  ContainerInfo* container = task.mutable_container();
  container->set_type(ContainerInfo::MESOS);

  // Make sure the container joins the mock CNI network.
  container->add_network_infos()->set_name("__MESOS_TEST__");

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished));

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(statusRunning);
  EXPECT_EQ(task.task_id(), statusRunning->task_id());
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  AWAIT_READY(statusFinished);
  EXPECT_EQ(task.task_id(), statusFinished->task_id());
  EXPECT_EQ(TASK_FINISHED, statusFinished->state());

  driver.stop();
  driver.join();
}


// This test verifies that we generate a /etc/resolv.conf
// that glibc accepts by using it to ping a host.
TEST_F(CniIsolatorTest, ROOT_INTERNET_VerifyResolverConfig)
{
  Try<net::IP::Network> hostNetwork = getNonLoopbackIP();
  ASSERT_SOME(hostNetwork);

  // Note: We set a dummy nameserver IP address followed by the
  // Google anycast address. We also set the resolver timeout
  // to 1sec so that ping doesn't time out waiting for DNS. Even
  // so, this test is probably susceptible to network flakiness,
  // especially in cloud providers.
  Try<string> mockPlugin = strings::format(
      R"~(
      #!/bin/sh
      echo '{'
      echo '  "ip4": {'
      echo '    "ip": "%s/%d"'
      echo '  },'
      echo '  "dns": {'
      echo '    "nameservers": ['
      echo '      "127.0.0.1",'
      echo '      "8.8.8.8"'
      echo '    ],'
      echo '    "domain": "mesos.apache.org",'
      echo '    "search": ['
      echo '      "a.mesos.apache.org",'
      echo '      "a.mesos.apache.org"'
      echo '    ],'
      echo '    "options":['
      echo '      "timeout:1"'
      echo '    ]'
      echo '  }'
      echo '}'
      )~",
      hostNetwork.get().address(),
      hostNetwork.get().prefix());

  ASSERT_SOME(mockPlugin);

  ASSERT_SOME(setupMockPlugin(mockPlugin.get()));

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

  MesosSchedulerDriver driver(
      &sched,
      DEFAULT_FRAMEWORK_INFO,
      master.get()->pid,
      DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  const Offer& offer = offers.get()[0];

  // In the CNI config above, we configured the Google
  // DNS servers as our second resolver. Verify that we
  // generated a resolv.conf that libc accepts by using
  // it to resolve www.google.com.
  const string command = R"~(
    #! /bin/sh
    set -ex
    exec ping -W 1 -c 2 www.google.com
    )~";

  TaskInfo task = createTask(offer, command);

  ContainerInfo* container = task.mutable_container();
  container->set_type(ContainerInfo::MESOS);

  // Make sure the container joins the mock CNI network.
  container->add_network_infos()->set_name("__MESOS_TEST__");

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished));

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(statusRunning);
  EXPECT_EQ(task.task_id(), statusRunning->task_id());
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());

  AWAIT_READY(statusFinished);
  EXPECT_EQ(task.task_id(), statusFinished->task_id());
  EXPECT_EQ(TASK_FINISHED, statusFinished->state());

  driver.stop();
  driver.join();
}


// This test launches a container which has an image and joins host
// network, and then verifies that /etc/hosts and friends are mounted
// read-only.
TEST_F(CniIsolatorTest, ROOT_INTERNET_CURL_ReadOnlyBindMounts)
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
      &sched,
      DEFAULT_FRAMEWORK_INFO,
      master.get()->pid,
      DEFAULT_CREDENTIAL);

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
  command.set_value("/bin/ash");
  command.add_arguments("ash");
  command.add_arguments("-c");
  command.add_arguments(
      "if echo '#sometext' >> /etc/resolv.conf; then"
      "  exit 1; "
      "else"
      "  exit 0; "
      "fi");

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


struct NetworkParam
{
  static NetworkParam host() { return NetworkParam(); }
  static NetworkParam named(const string& name)
  {
    NetworkParam param;
    param.networkInfo = v1::createNetworkInfo(name);
    return param;
  }

  Option<mesos::v1::NetworkInfo> networkInfo;
};


ostream& operator<<(ostream& stream, const NetworkParam& param)
{
  if (param.networkInfo.isSome()) {
    return stream << "Network '" << param.networkInfo->name() << "'";
  } else {
    return stream << "Host Network";
  }
}


class DefaultExecutorCniTest
  : public CniIsolatorTest,
    public WithParamInterface<NetworkParam>
{
protected:
  slave::Flags CreateSlaveFlags()
  {
    slave::Flags flags = CniIsolatorTest::CreateSlaveFlags();

    // Disable operator API authentication for the default executor.
    flags.authenticate_http_readwrite = false;
    flags.network_cni_plugins_dir = cniPluginDir;
    flags.network_cni_config_dir = cniConfigDir;

    return flags;
  }
};


// These tests are parameterized by the network on which the container
// is launched.
//
// TODO(asridharan): The version of gtest currently used by Mesos
// doesn't support passing `::testing::Values` a single value. Update
// these calls once we upgrade to a newer version.
INSTANTIATE_TEST_CASE_P(
    NetworkParam,
    DefaultExecutorCniTest,
    ::testing::Values(
        NetworkParam::host(),
        NetworkParam::named("__MESOS_TEST__")));


// This test verifies that the default executor sets the correct
// container IP when the container is launched on a host network or a
// CNI network.
//
// NOTE: To use the default executor, we will need to use the v1
// scheduler API.
TEST_P(DefaultExecutorCniTest, ROOT_VerifyContainerIP)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();

  Option<mesos::v1::NetworkInfo> networkInfo = GetParam().networkInfo;

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(DoAll(v1::scheduler::SendSubscribe(frameworkInfo),
                    FutureSatisfy(&connected)));

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(connected);

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId(subscribed->framework_id());

  v1::ExecutorInfo executorInfo = v1::createExecutorInfo(
      "test_default_executor",
      None(),
      "cpus:0.1;mem:32;disk:32",
      v1::ExecutorInfo::DEFAULT);

  // Update `executorInfo` with the subscribed `frameworkId`.
  executorInfo.mutable_framework_id()->CopyFrom(frameworkId);

  mesos::v1::ContainerInfo *container = executorInfo.mutable_container();
  container->set_type(mesos::v1::ContainerInfo::MESOS);

  if (networkInfo.isSome()) {
    container->add_network_infos()->CopyFrom(networkInfo.get());
  }

  AWAIT_READY(offers);
  ASSERT_NE(0, offers->offers().size());

  const v1::Offer& offer = offers->offers(0);
  const v1::AgentID& agentId = offer.agent_id();

  // The command tests if the MESOS_CONTAINER_IP is the same as the
  // `hostnetwork.address` which is what the mock CNI plugin would have
  // setup for the container.
  //
  // If the container is running on the host network we set the IP to
  // slave's PID, which is effectively the `LIBPROCESS_IP` that the
  // `DefaultExecutor` is going to see. If, however, the container is
  // running on a CNI network we choose the first non-loopback
  // address as `hostNetwork` since the mock CNI plugin
  // would set the container's IP to this address.
  Try<net::IP::Network> hostNetwork = net::IP::Network::create(
      slave.get()->pid.address.ip,
      32);

  if (networkInfo.isSome()) {
    hostNetwork = getNonLoopbackIP();
  }

  ASSERT_SOME(hostNetwork);

  string command = strings::format(
      R"~(
      #!/bin/sh
      if [ x"$MESOS_CONTAINER_IP" = x"%s" ]; then
        exit 0
      else
        exit 1
      fi)~",
      stringify(hostNetwork->address()),
      stringify(hostNetwork->address())).get();

  v1::TaskInfo taskInfo = v1::createTask(
      agentId,
      v1::Resources::parse("cpus:0.1;mem:32;disk:32").get(),
      command);

  Future<Event::Update> updateRunning;
  Future<Event::Update> updateFinished;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(DoAll(FutureArg<1>(&updateRunning),
                    v1::scheduler::SendAcknowledge(
                        frameworkId,
                        offer.agent_id())))
    .WillOnce(FutureArg<1>(&updateFinished));

  v1::Offer::Operation launchGroup = v1::LAUNCH_GROUP(
      executorInfo,
      v1::createTaskGroupInfo({taskInfo}));

  mesos.send(v1::createCallAccept(frameworkId, offer, {launchGroup}));

  AWAIT_READY(updateRunning);
  ASSERT_EQ(v1::TASK_RUNNING, updateRunning->status().state());
  EXPECT_EQ(taskInfo.task_id(), updateRunning->status().task_id());

  AWAIT_READY(updateFinished);
  ASSERT_EQ(v1::TASK_FINISHED, updateFinished->status().state());
  EXPECT_EQ(taskInfo.task_id(), updateFinished->status().task_id());
}


class CniIsolatorPortMapperTest : public CniIsolatorTest
{
public:
  virtual void SetUp()
  {
    CniIsolatorTest::SetUp();

    Try<string> mockConfig = os::read(
        path::join(cniConfigDir, MESOS_MOCK_CNI_CONFIG));

    ASSERT_SOME(mockConfig);

    // Create a CNI configuration to be used with the port-mapper plugin.
    Try<string> portMapperConfig = strings::format(R"~(
        {
          "name": "%s",
          "type": "mesos-cni-port-mapper",
          "chain": "%s",
          "delegate": %s
        }
        )~",
        MESOS_CNI_PORT_MAPPER_NETWORK,
        MESOS_TEST_PORT_MAPPER_CHAIN,
        mockConfig.get());

    ASSERT_SOME(portMapperConfig);

    Try<Nothing> write = os::write(
        path::join(cniConfigDir, "mockPortMapperConfig"),
        portMapperConfig.get());

    ASSERT_SOME(write);
  }

  virtual void TearDown()
  {
    // This is a best effort cleanup of the
    // `MESOS_TEST_PORT_MAPPER_CHAIN`. We shouldn't fail and bail on
    // rest of the `TearDown` if we are not able to clean up the
    // chain.
    string script = strings::format(
        R"~(
        #!/bin/sh
        set -x

        iptables -w -t nat --list %s

        if [ $? -eq 0 ]; then
          iptables -w -t nat -D OUTPUT ! -d 127.0.0.0/8 -m addrtype --dst-type LOCAL -j  %s
          iptables -w -t nat -D PREROUTING -m addrtype --dst-type LOCAL -j %s
          iptables -w -t nat -F %s
          iptables -w -t nat -X %s
        fi)~",
        stringify(MESOS_TEST_PORT_MAPPER_CHAIN),
        stringify(MESOS_TEST_PORT_MAPPER_CHAIN),
        stringify(MESOS_TEST_PORT_MAPPER_CHAIN),
        stringify(MESOS_TEST_PORT_MAPPER_CHAIN),
        stringify(MESOS_TEST_PORT_MAPPER_CHAIN),
        stringify(MESOS_TEST_PORT_MAPPER_CHAIN)).get();

    Try<string> result = os::shell(script);
    if (result.isError()) {
      LOG(ERROR) << "Unable to cleanup chain "
                 << stringify(MESOS_TEST_PORT_MAPPER_CHAIN)
                 << ": " << result.error();
    }

    CniIsolatorTest::TearDown();
  }
};


TEST_F(CniIsolatorPortMapperTest, ROOT_INTERNET_CURL_PortMapper)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "docker/runtime,filesystem/linux";
  flags.image_providers = "docker";
  flags.docker_store_dir = path::join(sandbox.get(), "store");

  // Augment the CNI plugins search path so that the `network/cni`
  // isolator can find the port-mapper CNI plugin.
  flags.network_cni_plugins_dir = cniPluginDir + ":" + getLauncherDir();
  flags.network_cni_config_dir = cniConfigDir;

  // Need to increase the registration timeout to give time for
  // downloading and provisioning the "nginx:alpine" image.
  flags.executor_registration_timeout = Minutes(5);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;

  MesosSchedulerDriver driver(
      &sched,
      DEFAULT_FRAMEWORK_INFO,
      master.get()->pid,
      DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  const Offer& offer = offers.get()[0];

  Resources resources(offers.get()[0].resources());

  // Make sure we have a `ports` resource.
  ASSERT_SOME(resources.ports());
  ASSERT_LE(1u, resources.ports()->range().size());

  // Select a random port from the offer.
  std::srand(std::time(0));
  Value::Range ports = resources.ports()->range(0);
  uint16_t hostPort =
    ports.begin() + std::rand() % (ports.end() - ports.begin() + 1);

  CommandInfo command;
  command.set_shell(false);

  TaskInfo task = createTask(
      offer.slave_id(),
      Resources::parse(
        "cpus:1;mem:128;"
        "ports:[" + stringify(hostPort) + "," + stringify(hostPort) + "]")
        .get(),
      command);

  ContainerInfo container = createContainerInfo("nginx:alpine");

  // Make sure the container joins the test CNI port-mapper network.
  NetworkInfo* networkInfo = container.add_network_infos();
  networkInfo->set_name(MESOS_CNI_PORT_MAPPER_NETWORK);

  NetworkInfo::PortMapping* portMapping = networkInfo->add_port_mappings();
  portMapping->set_container_port(80);
  portMapping->set_host_port(hostPort);

  // Set the container for the task.
  task.mutable_container()->CopyFrom(container);

  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning));

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY_FOR(statusRunning, Seconds(300));
  EXPECT_EQ(task.task_id(), statusRunning->task_id());
  EXPECT_EQ(TASK_RUNNING, statusRunning->state());
  ASSERT_TRUE(statusRunning->has_container_status());

  ContainerID containerId = statusRunning->container_status().container_id();
  ASSERT_EQ(1u, statusRunning->container_status().network_infos().size());

  // Try connecting to the nginx server on port 80 through a
  // non-loopback IP address on `hostPort`.
  Try<net::IP::Network> hostNetwork = getNonLoopbackIP();
  ASSERT_SOME(hostNetwork);

  // `TASK_RUNNING` does not guarantee that the service is running.
  // Hence, we need to re-try the service multiple times.
  Duration waited = Duration::zero();
  do {
    Try<string> connect = os::shell(
        "curl -I http://" + stringify(hostNetwork->address()) +
        ":" + stringify(hostPort));

    if (connect.isSome()) {
      LOG(INFO) << "Connection to nginx successful: " << connect.get();
      break;
    }

    os::sleep(Milliseconds(100));
    waited += Milliseconds(100);
  } while (waited < Seconds(10));

  EXPECT_LE(waited, Seconds(5));

  // Kill the task.
  Future<TaskStatus> statusKilled;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusKilled));

  // Wait for the executor to exit. We are using 'gc.schedule' as a
  // proxy event to monitor the exit of the executor.
  Future<Nothing> gcSchedule = FUTURE_DISPATCH(
      _, &slave::GarbageCollectorProcess::schedule);

  driver.killTask(task.task_id());

  AWAIT_READY(statusKilled);

  // The executor would issue a SIGTERM to the container, followed by
  // a SIGKILL (in case the container ignores the SIGTERM). The
  // "nginx:alpine" container returns an "EXIT_STATUS" of 0 on
  // receiving a SIGTERM making the executor send a `TASK_FINISHED`
  // instead of a `TASK_KILLED`, hence checking for `TASK_FINISHED`
  // instead of `TASK_KILLED`.
  EXPECT_EQ(TASK_FINISHED, statusKilled.get().state());

  AWAIT_READY(gcSchedule);

  // Make sure the iptables chain `MESOS-TEST-PORT-MAPPER-CHAIN`
  // doesn't have any iptable rules once the task is killed. The only
  // rule that should exist in this chain is the `-N
  // MESOS-TEST-PORT-MAPPER-CHAIN` rule.
  Try<string> rules = os::shell(
      "iptables -w -t nat -S " +
      stringify(MESOS_TEST_PORT_MAPPER_CHAIN) + "| wc -l");

  ASSERT_SOME(rules);
  ASSERT_EQ("1", strings::trim(rules.get()));

  driver.stop();
  driver.join();
}


class DefaultContainerDNSCniTest
  : public CniIsolatorTest,
    public WithParamInterface<string> {};


INSTANTIATE_TEST_CASE_P(
    DefaultContainerDNSInfo,
    DefaultContainerDNSCniTest,
    ::testing::Values(
        // A DNS information for the `__MESOS_TEST__` CNI network.
        "{\n"
        "  \"mesos\": [\n"
        "    {\n"
        "      \"network_mode\": \"CNI\",\n"
        "      \"network_name\": \"__MESOS_TEST__\",\n"
        "      \"dns\": {\n"
        "        \"nameservers\": [ \"8.8.8.8\", \"8.8.4.4\" ],\n"
        "        \"domain\": \"mesos.apache.org\",\n"
        "        \"search\": [ \"a.mesos.apache.org\" ],\n"
        "        \"options\": [ \"timeout:3\", \"attempts:2\" ]\n"
        "      }\n"
        "    }\n"
        "  ]\n"
        "}",
        // A DNS information with `network_mode == CNI`, but without a network
        // name, acts as a wildcard match making it the default DNS for any CNI
        // network not specified in the `--default_container_dns` flag.
        "{\n"
        "  \"mesos\": [\n"
        "    {\n"
        "      \"network_mode\": \"CNI\",\n"
        "      \"dns\": {\n"
        "        \"nameservers\": [ \"8.8.8.8\", \"8.8.4.4\" ],\n"
        "        \"domain\": \"mesos.apache.org\",\n"
        "        \"search\": [ \"a.mesos.apache.org\" ],\n"
        "        \"options\": [ \"timeout:3\", \"attempts:2\" ]\n"
        "      }\n"
        "    }\n"
        "  ]\n"
        "}",
        // Two DNS information, one is specific for `__MESOS_TEST__` CNI
        // network, the other is the defaule DNS for any CNI network not
        // specified in the `--default_container_dns` flag.
        "{\n"
        "  \"mesos\": [\n"
        "    {\n"
        "      \"network_mode\": \"CNI\",\n"
        "      \"network_name\": \"__MESOS_TEST__\",\n"
        "      \"dns\": {\n"
        "        \"nameservers\": [ \"8.8.8.8\", \"8.8.4.4\" ],\n"
        "        \"domain\": \"mesos.apache.org\",\n"
        "        \"search\": [ \"a.mesos.apache.org\" ],\n"
        "        \"options\": [ \"timeout:3\", \"attempts:2\" ]\n"
        "      }\n"
        "    },\n"
        "    {\n"
        "      \"network_mode\": \"CNI\",\n"
        "      \"dns\": {\n"
        "        \"nameservers\": [ \"8.8.8.9\", \"8.8.4.5\" ],\n"
        "        \"domain\": \"mesos1.apache.org\",\n"
        "        \"search\": [ \"b.mesos.apache.org\" ],\n"
        "        \"options\": [ \"timeout:9\", \"attempts:5\" ]\n"
        "      }\n"
        "    }\n"
        "  ]\n"
        "}"));


// This test verifies the DNS configuration of the container can be
// successfully set with the agent flag `--default_container_dns`.
TEST_P(DefaultContainerDNSCniTest, ROOT_VerifyDefaultDNS)
{
  Try<net::IP::Network> hostNetwork = getNonLoopbackIP();
  ASSERT_SOME(hostNetwork);

  Try<string> mockPlugin = strings::format(
      R"~(
      #!/bin/sh
      echo '{'
      echo '  "ip4": {'
      echo '    "ip": "%s/%d"'
      echo '  }'
      echo '}'
      )~",
      hostNetwork.get().address(),
      hostNetwork.get().prefix());

  ASSERT_SOME(mockPlugin);

  ASSERT_SOME(setupMockPlugin(mockPlugin.get()));

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "network/cni";

  flags.network_cni_plugins_dir = cniPluginDir;
  flags.network_cni_config_dir = cniConfigDir;

  Try<ContainerDNSInfo> parse = flags::parse<ContainerDNSInfo>(GetParam());
  ASSERT_SOME(parse);

  flags.default_container_dns = parse.get();

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;

  MesosSchedulerDriver driver(
      &sched,
      DEFAULT_FRAMEWORK_INFO,
      master.get()->pid,
      DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  const Offer& offer = offers.get()[0];

  // Verify that /etc/resolv.conf was generated the way we expect.
  // This is sensitive to changes in 'formatResolverConfig()'.
  const string command =
      "#! /bin/sh\n"
      "set -x\n"
      "cat > expected <<EOF\n"
      "domain mesos.apache.org\n"
      "search a.mesos.apache.org\n"
      "options timeout:3 attempts:2\n"
      "nameserver 8.8.8.8\n"
      "nameserver 8.8.4.4\n"
      "EOF\n"
      "cat /etc/resolv.conf\n"
      "exec diff -c /etc/resolv.conf expected\n";

  TaskInfo task = createTask(offer, command);

  ContainerInfo* container = task.mutable_container();
  container->set_type(ContainerInfo::MESOS);

  // Make sure the container joins the mock CNI network.
  container->add_network_infos()->set_name("__MESOS_TEST__");

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished));

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(statusRunning);
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
