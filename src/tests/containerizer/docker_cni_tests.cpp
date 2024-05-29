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
// WITHOUT WARRANTIES OR CONDITIONS OF ANY K:5IND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <gmock/gmock.h>

#include <gtest/gtest.h>

#include <mesos/v1/mesos.hpp>
#include <mesos/slave/container_logger.hpp>

#include <process/owned.hpp>

#include "slave/containerizer/docker.hpp"
#include "slave/containerizer/fetcher.hpp"
#include "slave/containerizer/mesos/isolators/network/cni/cni.hpp"

#include "slave/paths.hpp"
#include "slave/slave.hpp"
#include "slave/state.hpp"

#include "tests/flags.hpp"
#include "tests/mesos.hpp"
#include "tests/environment.hpp"
#include "tests/containerizer/docker_common.hpp"
#include "tests/mock_docker.hpp"


using namespace process;

using namespace mesos::internal::slave::paths;
using namespace mesos::internal::slave::state;

using mesos::internal::master::Master;
using mesos::internal::slave::DockerContainerizer;
using mesos::internal::slave::DockerContainerizerProcess;
using mesos::internal::slave::Fetcher;
using mesos::internal::slave::Slave;

using mesos::master::detector::MasterDetector;

using mesos::slave::ContainerConfig;
using mesos::slave::ContainerLogger;
using mesos::slave::ContainerTermination;

using std::list;
using std::string;
using std::vector;

using testing::_;
using testing::DoAll;
using testing::DoDefault;
using testing::Eq;
using testing::Invoke;
using testing::Return;

namespace mesos {
namespace internal {
namespace tests {

constexpr char MESOS_MOCK_CNI_CONFIG[] = "mockConfig";


static ContainerInfo createDockerInfo(const string& imageName)
{
    ContainerInfo containerInfo;

    containerInfo.set_type(ContainerInfo::DOCKER);
    containerInfo.mutable_docker()->set_image(imageName);

    return containerInfo;
}

class DockerCniTest : public MesosTest
{
public:
    string cniPluginDir;
    string cniConfigDir;

    void SetUp() override
    {
        MesosTest::SetUp();

        cniPluginDir = path::join(sandbox.get(), "plugins");
        cniConfigDir = path::join(sandbox.get(), "configs");

        // This is a veth CNI plugin that is written in bash. It creates a
        // veth virtual network pair, one end of the pair will be moved to
        // container network namespace.
        //
        // The veth CNI plugin uses 203.0.113.0/24 subnet, it is reserved
        // for documentation and examples [rfc5737]. The plugin can
        // allocate up to 128 veth pairs.
        Try<Nothing> result = setupMockPlugin(
            strings::format(R"~(
            #!/bin/sh
            set -e
            IP_ADDR="203.0.113.10/32"

            NETNS="mesos-test-veth-${PPID}"
            mkdir -p /var/run/netns
            cleanup() {
                rm -f /var/run/netns/$NETNS
            }
            trap cleanup EXIT
            ln -sf $CNI_NETNS /var/run/netns/$NETNS

            VETH0="vmesos1"
            VETH1="eth0"

            case $CNI_COMMAND in
            "ADD"*)
                ip link delete $VETH0 2>/dev/null || true
                ip netns exec $NETNS ip link delete $VETH1 2>/dev/null || true

                ip link add name $VETH0 type veth peer name $VETH1 > /tmp/cni.log || continue
                ip addr add "${IP_ADDR}" dev $VETH0 >> /tmp/cni.log
                ip link set $VETH0 up
                ip link set $VETH1 netns $NETNS
                ip netns exec $NETNS ip addr add "${IP_ADDR}" dev $VETH1
                ip netns exec $NETNS ip link set $VETH1 up

                echo "{"
                echo "  \"ip4\": {"
                echo "    \"ip\": \"${IP_ADDR}\""
                echo "  },"
                echo "  \"dns\": {"
                echo "    \"nameservers\": [ \"8.8.8.8\" ]"
                echo "  }"
                echo "}"
                ;;
            "DEL"*)
                # $VETH0 on host network namespace will be removed automatically.
                # If the plugin can't destroy the veth pair, it will be destroyed
                # with the container network namespace.
                ip link delete $VETH0 2>/dev/null || true
                ip netns exec $NETNS ip link del $VETH1 || true
                ;;
            esac
            )~").get());

        ASSERT_SOME(result);

        // Generate the mock CNI config for veth CNI plugin.
        ASSERT_SOME(os::mkdir(cniConfigDir));

        result = os::write(
            path::join(cniConfigDir, MESOS_MOCK_CNI_CONFIG),
            R"~(
            {
            "name": "__MESOS_TEST__",
            "type": "mockPlugin"
            })~");
    }

    // Generate the mock CNI plugin based on the given script.
    Try<Nothing> setupMockPlugin(const string& pluginScript)
    {
        return setupPlugin(pluginScript, "mockPlugin");
    }

    // Generate the CNI plugin based on the given script.
    Try<Nothing> setupPlugin(const string& pluginScript, const string& pluginName)
    {
      Try<Nothing> mkdir = os::mkdir(cniPluginDir);
      if (mkdir.isError()) {
        return Error("Failed to mkdir '" + cniPluginDir + "': " + mkdir.error());
      }

      string pluginPath = path::join(cniPluginDir, pluginName);

      Try<Nothing> write = os::write(pluginPath, pluginScript);
      if (write.isError()) {
        return Error("Failed to write '" + pluginPath + "': " + write.error());
      }

      // Make sure the plugin has execution permission.
      Try<Nothing> chmod = os::chmod(
          pluginPath,
          S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);

      if (chmod.isError()) {
        return Error("Failed to chmod '" + pluginPath + "': " + chmod.error());
      }

      return Nothing();
    }

    static string containerName(const ContainerID& containerId)
    {
      return slave::DOCKER_NAME_PREFIX + containerId.value();
    }

protected:
    slave::Flags CreateSlaveFlags() override
    {
        slave::Flags flags = MesosTest::CreateSlaveFlags();

        flags.network_cni_plugins_dir = cniPluginDir;
        flags.network_cni_config_dir = cniConfigDir;


        return flags;
    }
};

// This test tests the functionality of the docker's interfaces.
TEST_F(DockerCniTest, ROOT_DOCKER_ip_interface)
{
    SetUp();

    Try<Owned<cluster::Master>> master = StartMaster();
    ASSERT_SOME(master);

    MockDocker* mockDocker =
      new MockDocker(tests::flags.docker, tests::flags.docker_socket);

    Shared<Docker> docker(mockDocker);

    slave::Flags flags = CreateSlaveFlags();

    Fetcher fetcher(flags);

    Try<ContainerLogger*> logger =
      ContainerLogger::create(flags.container_logger);

    ASSERT_SOME(logger);

    MockDockerContainerizer dockerContainerizer(
        flags,
        &fetcher,
        Owned<ContainerLogger>(logger.get()),
        docker);

    Owned<MasterDetector> detector = master.get()->createDetector();

    Try<Owned<cluster::Slave>> slave =
      StartSlave(detector.get(), &dockerContainerizer, flags);
    ASSERT_SOME(slave);

    MockScheduler sched;
    MesosSchedulerDriver driver(
        &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

    Future<FrameworkID> frameworkId;
    EXPECT_CALL(sched, registered(&driver, _, _))
      .WillOnce(FutureArg<1>(&frameworkId));

    Future<vector<Offer>> offers;
    EXPECT_CALL(sched, resourceOffers(&driver, _))
      .WillOnce(FutureArg<1>(&offers))
      .WillRepeatedly(Return()); // Ignore subsequent offers.

    driver.start();

    AWAIT_READY(frameworkId);

    AWAIT_READY(offers);
    ASSERT_FALSE(offers->empty());

    TaskInfo task = createTask(
        offers->front().slave_id(),
        offers->front().resources(),
        "sleep 10; ping 203.0.113.10 -c 1 -q");

    ContainerInfo containerInfo = createDockerInfo("docker.io/alpine:latest");

    containerInfo.mutable_docker()->set_network(
        ContainerInfo::DockerInfo::BRIDGE);

    task.mutable_container()->CopyFrom(containerInfo);
    task.mutable_container()->add_network_infos()->set_name("__MESOS_TEST__");

    Future<ContainerID> containerId;
    Future<ContainerConfig> containerConfig;
    EXPECT_CALL(dockerContainerizer, launch(_, _, _, _))
      .WillOnce(DoAll(FutureArg<0>(&containerId),
                      FutureArg<1>(&containerConfig),
                      Invoke(&dockerContainerizer,
                             &MockDockerContainerizer::_launch)));

    Future<TaskStatus> statusStarting;
    Future<TaskStatus> statusRunning;
    Future<TaskStatus> statusFinished;
    EXPECT_CALL(sched, statusUpdate(&driver, _))
      .WillOnce(FutureArg<1>(&statusStarting))
      .WillOnce(FutureArg<1>(&statusRunning))
      .WillOnce(FutureArg<1>(&statusFinished))
      .WillRepeatedly(DoDefault());

    driver.launchTasks(offers.get()[0].id(), {task});

    AWAIT_READY_FOR(containerId, Seconds(60));
    AWAIT_READY_FOR(statusStarting, Seconds(60));
    EXPECT_EQ(TASK_STARTING, statusStarting->state());
    AWAIT_READY_FOR(statusRunning, Seconds(60));
    EXPECT_EQ(TASK_RUNNING, statusRunning->state());
    ASSERT_TRUE(statusRunning->has_data());
    AWAIT_READY_FOR(statusFinished, Seconds(60));
    EXPECT_EQ(TASK_FINISHED, statusFinished->state());

    Future<Option<ContainerTermination>> termination =
      dockerContainerizer.wait(containerId.get());

    driver.stop();
    driver.join();

    AWAIT_READY(termination);
    EXPECT_SOME(termination.get());
}

} // namespace tests
} // namespace internal
} // namespace mesos
