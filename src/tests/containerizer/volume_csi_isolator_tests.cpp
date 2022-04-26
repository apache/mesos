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

#include <list>
#include <string>
#include <vector>

#include <mesos/mesos.hpp>

#include <mesos/authentication/secret_generator.hpp>

#include <mesos/csi/v0.hpp>
#include <mesos/csi/v1.hpp>

#include <mesos/master/detector.hpp>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/grpc.hpp>
#include <process/owned.hpp>
#include <process/reap.hpp>
#include <process/subprocess.hpp>

#include <stout/bytes.hpp>
#include <stout/hashmap.hpp>
#include <stout/path.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/stringify.hpp>

#include <stout/tests/utils.hpp>

#ifdef USE_SSL_SOCKET
#include "authentication/executor/jwt_secret_generator.hpp"
#endif // USE_SSL_SOCKET

#include "csi/paths.hpp"

#include "master/flags.hpp"

#include "slave/csi_server.hpp"
#include "slave/flags.hpp"
#include "slave/paths.hpp"

#include "slave/containerizer/fetcher.hpp"

#include "slave/containerizer/mesos/containerizer.hpp"
#include "slave/containerizer/mesos/paths.hpp"

#include "tests/environment.hpp"
#include "tests/mesos.hpp"

#ifdef USE_SSL_SOCKET
using mesos::authentication::executor::JWTSecretGenerator;
#endif // USE_SSL_SOCKET

using mesos::internal::slave::CSIServer;
using mesos::internal::slave::Fetcher;
using mesos::internal::slave::MesosContainerizer;

using mesos::internal::slave::containerizer::paths::getContainerPid;

using mesos::master::detector::MasterDetector;

using process::Clock;
using process::Future;
using process::Owned;

using std::list;
using std::string;
using std::vector;

using testing::AllOf;
using testing::AnyOf;
using testing::DoAll;

namespace mesos {
namespace internal {
namespace tests {

const string TEST_CONTAINER_PATH = "volume-container-path/";
const string TEST_CSI_PLUGIN_TYPE = "org.apache.mesos.csi.test";
const string TEST_OUTPUT_FILE = "output.txt";
const string TEST_OUTPUT_STRING = "hello world";
const string TEST_VOLUME_ID = "test-volume";


class VolumeCSIIsolatorTest
  : public MesosTest,
    public testing::WithParamInterface<string>
{
public:
  void SetUp() override
  {
    MesosTest::SetUp();

    csiPluginConfigDir = path::join(sandbox.get(), "csi_plugin_configs");

    ASSERT_SOME(os::mkdir(csiPluginConfigDir));
  }

  master::Flags CreateMasterFlags() override
  {
    master::Flags flags = MesosTest::CreateMasterFlags();

    // This extended allocation interval helps us avoid unwanted allocations
    // when running the clock.
    flags.allocation_interval = Seconds(99);

    return flags;
  }

  slave::Flags CreateSlaveFlags() override
  {
    slave::Flags flags = MesosTest::CreateSlaveFlags();

    flags.csi_plugin_config_dir = csiPluginConfigDir;
    flags.image_providers = "DOCKER";
    flags.isolation =
      flags.isolation + ",filesystem/linux,volume/csi,docker/runtime";

    agentWorkDir = flags.work_dir;

    return flags;
  }

  void TearDown() override
  {
    const string csiRootDir = slave::paths::getCsiRootDir(agentWorkDir);

    Try<list<string>> csiContainerPaths =
      csi::paths::getContainerPaths(csiRootDir, "*", "*");

    ASSERT_SOME(csiContainerPaths);

    foreach (const string& path, csiContainerPaths.get()) {
      Try<csi::paths::ContainerPath> containerPath =
        csi::paths::parseContainerPath(csiRootDir, path);

      ASSERT_SOME(containerPath);

      Result<string> endpointDir =
        os::realpath(csi::paths::getEndpointDirSymlinkPath(
            csiRootDir,
            containerPath->type,
            containerPath->name,
            containerPath->containerId));

      if (endpointDir.isSome()) {
        ASSERT_SOME(os::rmdir(endpointDir.get()));
      }
    }
  }

  void createCsiPluginConfig(
      const Bytes& capacity,
      const Option<string>& volumes = None(),
      const string& name = TEST_CSI_PLUGIN_TYPE)
  {
    Try<string> mkdtemp = environment->mkdtemp();
    ASSERT_SOME(mkdtemp);

    const string& testCsiPluginWorkDir = mkdtemp.get();

    const string testCsiPluginPath =
      path::join(getTestHelperDir(), "test-csi-plugin");

    // Note that the `--endpoint` flag required by the test CSI plugin is
    // injected by the ServiceManager.
    Try<string> csiPluginConfig = strings::format(
        R"~(
        {
          "type": "%s",
          "containers": [
            {
              "services": [
                "NODE_SERVICE"
              ],
              "command": {
                "shell": false,
                "value": "%s",
                "arguments": [
                  "%s",
                  "--work_dir=%s",
                  "--available_capacity=%s",
                  "%s",
                  "--volume_id_path=false",
                  "--api_version=%s"
                ]
              },
              "resources": [
                {
                  "name": "cpus",
                  "type": "SCALAR",
                  "scalar": {
                    "value": 0.1
                  }
                },
                {
                  "name": "mem",
                  "type": "SCALAR",
                  "scalar": {
                    "value": 1024
                  }
                }
              ]
            }
          ]
        }
        )~",
        name,
        testCsiPluginPath,
        testCsiPluginPath,
        testCsiPluginWorkDir,
        stringify(capacity),
        volumes.isSome() ? "--volumes=" + volumes.get() : "",
        GetParam());

    ASSERT_SOME(csiPluginConfig);
    ASSERT_SOME(os::write(
        path::join(csiPluginConfigDir, name + ".json"),
        csiPluginConfig.get()));
  }

  SecretGenerator* createSecretGenerator(const slave::Flags& agentFlags)
  {
    SecretGenerator* secretGenerator = nullptr;

#ifdef USE_SSL_SOCKET
    CHECK_SOME(agentFlags.jwt_secret_key);

    Try<string> jwtSecretKey = os::read(agentFlags.jwt_secret_key.get());
    if (jwtSecretKey.isError()) {
      EXIT(EXIT_FAILURE) << "Failed to read the file specified by "
                         << "--jwt_secret_key";
    }

    Try<os::Permissions> permissions =
      os::permissions(agentFlags.jwt_secret_key.get());
    if (permissions.isError()) {
      LOG(WARNING) << "Failed to stat jwt secret key file '"
                   << agentFlags.jwt_secret_key.get()
                   << "': " << permissions.error();
    } else if (permissions->others.rwx) {
      LOG(WARNING) << "Permissions on executor secret key file '"
                   << agentFlags.jwt_secret_key.get()
                   << "' are too open; it is recommended that your"
                   << " key file is NOT accessible by others";
    }

    secretGenerator = new JWTSecretGenerator(jwtSecretKey.get());
#endif // USE_SSL_SOCKET

    return secretGenerator;
  }

  string csiPluginConfigDir;
  string agentWorkDir;
};


INSTANTIATE_TEST_CASE_P(
    CSIVersion,
    VolumeCSIIsolatorTest,
    testing::Values(csi::v0::API_VERSION, csi::v1::API_VERSION),
    [](const testing::TestParamInfo<string>& info) { return info.param; });


// When one or more invalid plugin configurations exist in the config
// directory, the CSI server's `start()` method should return a failure.
TEST_P(VolumeCSIIsolatorTest, ROOT_InvalidPluginConfig)
{
  // Write an invalid configuration file to the config directory.
  ASSERT_SOME(os::write(
      path::join(csiPluginConfigDir, "config.json"),
      "this is not valid JSON"));

  process::http::URL agentURL(
      "http",
      process::address().ip,
      process::address().port,
      "slave/api/v1");

  Try<Owned<CSIServer>> server =
    CSIServer::create(CreateSlaveFlags(), agentURL, nullptr, nullptr);

  ASSERT_SOME(server);

  SlaveID agentId;

  agentId.set_value("0123456789");

  Future<Nothing> started = server.get()->start(agentId);

  AWAIT_FAILED(started);

  ASSERT_TRUE(strings::contains(
      started.failure(),
      "CSI server failed to initialize CSI plugins: JSON parse"));
}


// To verify the basic functionality of CSI volumes, we launch one task which
// mounts a preprovisioned volume and writes to it. Then, we launch a second
// task which reads and verifies the output from the same volume.
TEST_P(VolumeCSIIsolatorTest, ROOT_INTERNET_CURL_CommandTaskWithVolume)
{
  createCsiPluginConfig(Bytes(0), TEST_VOLUME_ID + ":1MB");

  master::Flags masterFlags = CreateMasterFlags();

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags agentFlags = CreateSlaveFlags();

  const SlaveOptions agentOptions =
    SlaveOptions(detector.get())
      .withFlags(agentFlags);

  Try<Owned<cluster::Slave>> agent = StartSlave(agentOptions);
  ASSERT_SOME(agent);

  AWAIT_READY(slaveRegisteredMessage);

  Clock::pause();

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(v1::DEFAULT_FRAMEWORK_INFO));

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(subscribed);
  v1::FrameworkID frameworkId(subscribed->framework_id());

  v1::Resources resources =
    v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  v1::ExecutorInfo executorInfo = v1::createExecutorInfo(
      v1::DEFAULT_EXECUTOR_ID,
      None(),
      resources,
      v1::ExecutorInfo::DEFAULT,
      frameworkId);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  v1::Offer offer = offers->offers(0);
  const v1::AgentID& agentId = offer.agent_id();

  Try<string> taskCommand = strings::format(
      "echo '%s' > %s",
      TEST_OUTPUT_STRING,
      TEST_CONTAINER_PATH + TEST_OUTPUT_FILE);

  v1::TaskInfo taskInfo = v1::createTask(agentId, resources, taskCommand.get());

  taskInfo.mutable_container()->CopyFrom(v1::createContainerInfo(
      "alpine",
      {v1::createVolumeCsi(
          TEST_CSI_PLUGIN_TYPE,
          TEST_VOLUME_ID,
          TEST_CONTAINER_PATH,
          mesos::v1::Volume::RW,
          mesos::v1::Volume::Source::CSIVolume::VolumeCapability
            ::AccessMode::SINGLE_NODE_WRITER)}));

  Future<v1::scheduler::Event::Update> startingUpdate;
  Future<v1::scheduler::Event::Update> runningUpdate;
  Future<v1::scheduler::Event::Update> finishedUpdate;

  testing::Sequence taskSequence;
  EXPECT_CALL(
      *scheduler,
      update(_, TaskStatusUpdateStateEq(v1::TASK_STARTING)))
    .InSequence(taskSequence)
    .WillOnce(DoAll(
        FutureArg<1>(&startingUpdate),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillRepeatedly(v1::scheduler::SendAcknowledge(frameworkId, agentId));

  EXPECT_CALL(
      *scheduler,
      update(_, TaskStatusUpdateStateEq(v1::TASK_RUNNING)))
    .InSequence(taskSequence)
    .WillOnce(DoAll(
        FutureArg<1>(&runningUpdate),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillRepeatedly(v1::scheduler::SendAcknowledge(frameworkId, agentId));

  EXPECT_CALL(
      *scheduler,
      update(_, TaskStatusUpdateStateEq(v1::TASK_FINISHED)))
    .InSequence(taskSequence)
    .WillOnce(DoAll(
        FutureArg<1>(&finishedUpdate),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillRepeatedly(v1::scheduler::SendAcknowledge(frameworkId, agentId));

  Clock::resume();

  mesos.send(
      v1::createCallAccept(
          frameworkId,
          offer,
          {v1::LAUNCH({taskInfo})}));

  AWAIT_READY(startingUpdate);
  AWAIT_READY(runningUpdate);
  AWAIT_READY(finishedUpdate);

  // To avoid pausing the clock before the acknowledgement is sent, we await on
  // this future.
  Future<StatusUpdateAcknowledgementMessage> acknowledgement =
    FUTURE_PROTOBUF(StatusUpdateAcknowledgementMessage(), _, _);

  AWAIT_READY(acknowledgement);

  Clock::pause();

  // Run another task which mounts the same external volume and reads the
  // output of the previous task.
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers));

  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  offer = offers->offers(0);

  taskCommand = strings::format(
      "if [ \"`cat %s`\" = \"%s\" ]; then exit 0; else exit 1; fi",
      TEST_CONTAINER_PATH + TEST_OUTPUT_FILE,
      TEST_OUTPUT_STRING);

  CHECK_SOME(taskCommand);

  taskInfo = v1::createTask(agentId, resources, taskCommand.get());

  taskInfo.mutable_container()->CopyFrom(v1::createContainerInfo(
      None(),
      {v1::createVolumeCsi(
          TEST_CSI_PLUGIN_TYPE,
          TEST_VOLUME_ID,
          TEST_CONTAINER_PATH,
          mesos::v1::Volume::RW,
          mesos::v1::Volume::Source::CSIVolume::VolumeCapability
            ::AccessMode::SINGLE_NODE_WRITER)}));

  EXPECT_CALL(
      *scheduler,
      update(_, TaskStatusUpdateStateEq(v1::TASK_STARTING)))
    .InSequence(taskSequence)
    .WillOnce(DoAll(
        FutureArg<1>(&startingUpdate),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillRepeatedly(v1::scheduler::SendAcknowledge(frameworkId, agentId));

  EXPECT_CALL(
      *scheduler,
      update(_, TaskStatusUpdateStateEq(v1::TASK_RUNNING)))
    .InSequence(taskSequence)
    .WillOnce(DoAll(
        FutureArg<1>(&runningUpdate),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillRepeatedly(v1::scheduler::SendAcknowledge(frameworkId, agentId));

  EXPECT_CALL(
      *scheduler,
      update(_, TaskStatusUpdateStateEq(v1::TASK_FINISHED)))
    .InSequence(taskSequence)
    .WillOnce(DoAll(
        FutureArg<1>(&finishedUpdate),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillRepeatedly(v1::scheduler::SendAcknowledge(frameworkId, agentId));

  Clock::resume();

  mesos.send(
      v1::createCallAccept(
          frameworkId,
          offer,
          {v1::LAUNCH({taskInfo})}));

  AWAIT_READY(startingUpdate);
  AWAIT_READY(runningUpdate);
  AWAIT_READY(finishedUpdate);
}


// Two tasks in a task group should both be able to mount the same CSI volume.
TEST_P(VolumeCSIIsolatorTest, ROOT_INTERNET_CURL_TaskGroupWithVolume)
{
  createCsiPluginConfig(Bytes(0), TEST_VOLUME_ID + ":1MB");

  master::Flags masterFlags = CreateMasterFlags();

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags agentFlags = CreateSlaveFlags();

  const SlaveOptions agentOptions =
    SlaveOptions(detector.get())
      .withFlags(agentFlags);

  Try<Owned<cluster::Slave>> agent = StartSlave(agentOptions);
  ASSERT_SOME(agent);

  AWAIT_READY(slaveRegisteredMessage);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(v1::DEFAULT_FRAMEWORK_INFO));

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(subscribed);
  v1::FrameworkID frameworkId(subscribed->framework_id());

  v1::Resources resources =
    v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  v1::ExecutorInfo executorInfo = v1::createExecutorInfo(
      v1::DEFAULT_EXECUTOR_ID,
      None(),
      resources,
      v1::ExecutorInfo::DEFAULT,
      frameworkId);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  v1::Offer offer = offers->offers(0);
  const v1::AgentID& agentId = offer.agent_id();

  Try<string> taskCommand1 = strings::format(
      "echo '%s' > %s && exit 0",
      TEST_OUTPUT_STRING,
      TEST_CONTAINER_PATH + TEST_OUTPUT_FILE);

  Try<string> taskCommand2 = strings::format(
      "while [ \"`cat %s 2>/dev/null`\" != \"%s\" ]; do : "
      "sleep 0.1 ; done; exit 0",
      TEST_CONTAINER_PATH + TEST_OUTPUT_FILE,
      TEST_OUTPUT_STRING);

  v1::TaskInfo taskInfo1 =
    v1::createTask(agentId, resources, taskCommand1.get());

  v1::TaskInfo taskInfo2 =
    v1::createTask(agentId, resources, taskCommand2.get());

  mesos::v1::Volume volume = v1::createVolumeCsi(
      TEST_CSI_PLUGIN_TYPE,
      TEST_VOLUME_ID,
      TEST_CONTAINER_PATH,
      mesos::v1::Volume::RW,
      mesos::v1::Volume::Source::CSIVolume::VolumeCapability
        ::AccessMode::SINGLE_NODE_WRITER);

  taskInfo1.mutable_container()->CopyFrom(
      v1::createContainerInfo("alpine", {volume}));

  taskInfo2.mutable_container()->CopyFrom(
      v1::createContainerInfo(None(), {volume}));

  Future<v1::scheduler::Event::Update> startingUpdate1;
  Future<v1::scheduler::Event::Update> runningUpdate1;
  Future<v1::scheduler::Event::Update> finishedUpdate1;

  testing::Sequence task1;
  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(taskInfo1.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_STARTING))))
    .InSequence(task1)
    .WillOnce(DoAll(
        FutureArg<1>(&startingUpdate1),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillRepeatedly(v1::scheduler::SendAcknowledge(frameworkId, agentId));

  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(taskInfo1.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_RUNNING))))
    .InSequence(task1)
    .WillOnce(DoAll(
        FutureArg<1>(&runningUpdate1),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillRepeatedly(v1::scheduler::SendAcknowledge(frameworkId, agentId));

  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(taskInfo1.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_FINISHED))))
    .InSequence(task1)
    .WillOnce(DoAll(
        FutureArg<1>(&finishedUpdate1),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillRepeatedly(v1::scheduler::SendAcknowledge(frameworkId, agentId));

  Future<v1::scheduler::Event::Update> startingUpdate2;
  Future<v1::scheduler::Event::Update> runningUpdate2;
  Future<v1::scheduler::Event::Update> finishedUpdate2;

  testing::Sequence task2;
  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(taskInfo2.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_STARTING))))
    .InSequence(task2)
    .WillOnce(DoAll(
        FutureArg<1>(&startingUpdate2),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillRepeatedly(v1::scheduler::SendAcknowledge(frameworkId, agentId));

  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(taskInfo2.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_RUNNING))))
    .InSequence(task2)
    .WillOnce(DoAll(
        FutureArg<1>(&runningUpdate2),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillRepeatedly(v1::scheduler::SendAcknowledge(frameworkId, agentId));

  EXPECT_CALL(
      *scheduler,
      update(_, AllOf(
          TaskStatusUpdateTaskIdEq(taskInfo2.task_id()),
          TaskStatusUpdateStateEq(v1::TASK_FINISHED))))
    .InSequence(task2)
    .WillOnce(DoAll(
        FutureArg<1>(&finishedUpdate2),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillRepeatedly(v1::scheduler::SendAcknowledge(frameworkId, agentId));

  mesos.send(
      v1::createCallAccept(
          frameworkId,
          offer,
          {v1::LAUNCH_GROUP(
              executorInfo, v1::createTaskGroupInfo({taskInfo1, taskInfo2}))}));

  AWAIT_READY(startingUpdate1);
  AWAIT_READY(startingUpdate2);

  AWAIT_READY(runningUpdate1);
  AWAIT_READY(runningUpdate2);

  AWAIT_READY(finishedUpdate1);
  AWAIT_READY(finishedUpdate2);
}


// A task which is executed as a non-root user should be able to mount a CSI
// volume and write to it.
TEST_P(VolumeCSIIsolatorTest, UNPRIVILEGED_USER_NonRootTaskUser)
{
  createCsiPluginConfig(Bytes(0), TEST_VOLUME_ID + ":1MB");

  master::Flags masterFlags = CreateMasterFlags();

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags agentFlags = CreateSlaveFlags();

  const SlaveOptions agentOptions =
    SlaveOptions(detector.get())
      .withFlags(agentFlags);

  Try<Owned<cluster::Slave>> agent = StartSlave(agentOptions);
  ASSERT_SOME(agent);

  AWAIT_READY(slaveRegisteredMessage);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(v1::DEFAULT_FRAMEWORK_INFO));

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(subscribed);
  v1::FrameworkID frameworkId(subscribed->framework_id());

  v1::Resources resources =
    v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  v1::ExecutorInfo executorInfo = v1::createExecutorInfo(
      v1::DEFAULT_EXECUTOR_ID,
      None(),
      resources,
      v1::ExecutorInfo::DEFAULT,
      frameworkId);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  v1::Offer offer = offers->offers(0);
  const v1::AgentID& agentId = offer.agent_id();

  v1::CommandInfo taskCommand = v1::createCommandInfo(
      strings::format(
          "echo '%s' > %s && "
            "if [ \"`cat %s`\" = \"%s\" ] ; then exit 0 ; else exit 1 ; fi",
          TEST_OUTPUT_STRING,
          TEST_CONTAINER_PATH + TEST_OUTPUT_FILE,
          TEST_CONTAINER_PATH + TEST_OUTPUT_FILE,
          TEST_OUTPUT_STRING).get());

  Option<string> user = os::getenv("SUDO_USER");
  ASSERT_SOME(user);

  taskCommand.set_user(user.get());

  v1::TaskInfo taskInfo = v1::createTask(agentId, resources, taskCommand);

  taskInfo.mutable_container()->CopyFrom(v1::createContainerInfo(
      None(),
      {v1::createVolumeCsi(
          TEST_CSI_PLUGIN_TYPE,
          TEST_VOLUME_ID,
          TEST_CONTAINER_PATH,
          mesos::v1::Volume::RW,
          mesos::v1::Volume::Source::CSIVolume::VolumeCapability
            ::AccessMode::SINGLE_NODE_WRITER)}));

  Future<v1::scheduler::Event::Update> startingUpdate;
  Future<v1::scheduler::Event::Update> runningUpdate;
  Future<v1::scheduler::Event::Update> finishedUpdate;

  testing::Sequence taskSequence;
  EXPECT_CALL(*scheduler, update(_, TaskStatusUpdateStateEq(v1::TASK_STARTING)))
    .InSequence(taskSequence)
    .WillOnce(DoAll(
        FutureArg<1>(&startingUpdate),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillRepeatedly(v1::scheduler::SendAcknowledge(frameworkId, agentId));

  EXPECT_CALL(*scheduler, update(_, TaskStatusUpdateStateEq(v1::TASK_RUNNING)))
    .InSequence(taskSequence)
    .WillOnce(DoAll(
        FutureArg<1>(&runningUpdate),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillRepeatedly(v1::scheduler::SendAcknowledge(frameworkId, agentId));

  EXPECT_CALL(*scheduler, update(_, TaskStatusUpdateStateEq(v1::TASK_FINISHED)))
    .InSequence(taskSequence)
    .WillOnce(DoAll(
        FutureArg<1>(&finishedUpdate),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillRepeatedly(v1::scheduler::SendAcknowledge(frameworkId, agentId));

  mesos.send(v1::createCallAccept(
      frameworkId,
      offer,
      {v1::LAUNCH({taskInfo})}));

  AWAIT_READY(startingUpdate);
  AWAIT_READY(runningUpdate);
  AWAIT_READY(finishedUpdate);
}


// If a publish call is made against a plugin whose configuration file is not
// present, the call should fail. If a valid configuration is added for the
// plugin later, it should be initialized and the call should be handled.
TEST_P(VolumeCSIIsolatorTest, ROOT_PluginConfigAddedAtRuntime)
{
  // Write a default plugin configuration to disk so that the CSI server will
  // initialize successfully.
  createCsiPluginConfig(Bytes(0), TEST_VOLUME_ID + ":1MB");

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();

  const slave::Flags agentFlags = CreateSlaveFlags();

  const string processId = process::ID::generate("slave");

  const process::http::URL agentUrl(
      "http",
      process::address().ip,
      process::address().port,
      processId + "/api/v1");

  Try<Owned<CSIServer>> csiServer = CSIServer::create(
      agentFlags, agentUrl, createSecretGenerator(agentFlags), nullptr);

  ASSERT_SOME(csiServer);

  auto agentOptions =
    SlaveOptions(detector.get())
      .withId(processId)
      .withFlags(agentFlags)
      .withCsiServer(csiServer.get());

  Try<Owned<cluster::Slave>> agent = StartSlave(agentOptions);
  ASSERT_SOME(agent);

  AWAIT_READY(slaveRegisteredMessage);

  SlaveID agentId = slaveRegisteredMessage->slave_id();

  Future<Nothing> started = csiServer.get()->start(agentId);

  AWAIT_READY(started);

  Volume::Source::CSIVolume::VolumeCapability capability;
  capability.mutable_mount();
  capability.mutable_access_mode()->set_mode(
      Volume::Source::CSIVolume::VolumeCapability
      ::AccessMode::SINGLE_NODE_WRITER);

  Volume::Source::CSIVolume::StaticProvisioning staticVol;
  staticVol.set_volume_id(TEST_VOLUME_ID);
  staticVol.mutable_volume_capability()->CopyFrom(capability);

  const string pluginName = "org.apache.mesos.csi.added-at-runtime";

  Volume::Source::CSIVolume csiVolume;
  csiVolume.set_plugin_name(pluginName);
  csiVolume.mutable_static_provisioning()->CopyFrom(staticVol);

  Volume volume;
  Volume::Source* source = volume.mutable_source();
  source->set_type(Volume::Source::CSI_VOLUME);
  source->mutable_csi_volume()->CopyFrom(csiVolume);

  // First, perform publish/unpublish calls before we have written the
  // configuration to disk.
  Future<string> nodePublishResult = csiServer.get()->publishVolume(volume);

  AWAIT_FAILED(nodePublishResult);

  ASSERT_TRUE(strings::contains(
      nodePublishResult.failure(),
      "Failed to initialize CSI plugin '" + pluginName +
        "': No valid CSI plugin configurations found"));

  // Now write the configuration to disk and try the calls again.
  createCsiPluginConfig(Bytes(0), TEST_VOLUME_ID + ":1MB", pluginName);

  nodePublishResult = csiServer.get()->publishVolume(volume);

  AWAIT_READY(nodePublishResult);

  Future<Nothing> nodeUnpublishResult =
    csiServer.get()->unpublishVolume(pluginName, TEST_VOLUME_ID);

  AWAIT_READY(nodeUnpublishResult);
}


// This test verifies basic functionality of a CSI volume when it is published
// by an unmanaged CSI plugin.
TEST_P(VolumeCSIIsolatorTest, ROOT_UnmanagedPlugin)
{
  const string pluginBinary = "test-csi-plugin";

  // Launch the test CSI plugin manually in a subprocess.
  Try<string> mkdtemp = environment->mkdtemp();
  ASSERT_SOME(mkdtemp);

  const string& testCsiPluginWorkDir = mkdtemp.get();

  const string testCsiPluginPath = path::join(getTestHelperDir(), pluginBinary);

  const string testCsiSocketPath =
    path::join("unix:///", testCsiPluginWorkDir, "endpoint.sock");

  vector<string> argv {
    pluginBinary,
    "--work_dir=" + testCsiPluginWorkDir,
    "--available_capacity=" + stringify(Bytes(0)),
    "--volumes=" + TEST_VOLUME_ID + ":1MB",
    "--volume_id_path=false",
    "--endpoint=" + testCsiSocketPath,
    "--api_version=" + GetParam()
  };

  Result<string> path = os::realpath(getLauncherDir());
  ASSERT_SOME(path);

  Try<process::Subprocess> csiPlugin = process::subprocess(
        path::join(path.get(), pluginBinary),
        argv,
        process::Subprocess::FD(STDIN_FILENO),
        process::Subprocess::FD(STDOUT_FILENO),
        process::Subprocess::FD(STDERR_FILENO),
        nullptr, // Don't pass flags.
        None(),  // No environment.
        None(),  // Use default clone.
        {},      // No parent hooks.
        { process::Subprocess::ChildHook::SETSID() });

  ASSERT_SOME(csiPlugin);

  Try<string> csiPluginConfig = strings::format(
      R"~(
      {
        "type": "%s",
        "endpoints": [
          {
            "csi_service": "NODE_SERVICE",
            "endpoint": "%s"
          }
        ]
      }
      )~",
      TEST_CSI_PLUGIN_TYPE,
      testCsiSocketPath);

  ASSERT_SOME(csiPluginConfig);
  ASSERT_SOME(os::write(
      path::join(csiPluginConfigDir, "plugin_config.json"),
      csiPluginConfig.get()));

  master::Flags masterFlags = CreateMasterFlags();

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags agentFlags = CreateSlaveFlags();

  const SlaveOptions agentOptions =
    SlaveOptions(detector.get())
      .withFlags(agentFlags);

  Try<Owned<cluster::Slave>> agent = StartSlave(agentOptions);
  ASSERT_SOME(agent);

  AWAIT_READY(slaveRegisteredMessage);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(v1::DEFAULT_FRAMEWORK_INFO));

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(subscribed);
  v1::FrameworkID frameworkId(subscribed->framework_id());

  v1::Resources resources =
    v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  v1::ExecutorInfo executorInfo = v1::createExecutorInfo(
      v1::DEFAULT_EXECUTOR_ID,
      None(),
      resources,
      v1::ExecutorInfo::DEFAULT,
      frameworkId);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  v1::Offer offer = offers->offers(0);
  const v1::AgentID& agentId = offer.agent_id();

  v1::CommandInfo taskCommand = v1::createCommandInfo(
      strings::format(
          "echo '%s' > %s && "
            "if [ \"`cat %s`\" = \"%s\" ] ; then exit 0 ; else exit 1 ; fi",
          TEST_OUTPUT_STRING,
          TEST_CONTAINER_PATH + TEST_OUTPUT_FILE,
          TEST_CONTAINER_PATH + TEST_OUTPUT_FILE,
          TEST_OUTPUT_STRING).get());

  v1::TaskInfo taskInfo = v1::createTask(agentId, resources, taskCommand);

  taskInfo.mutable_container()->CopyFrom(v1::createContainerInfo(
      None(),
      {v1::createVolumeCsi(
          TEST_CSI_PLUGIN_TYPE,
          TEST_VOLUME_ID,
          TEST_CONTAINER_PATH,
          mesos::v1::Volume::RW,
          mesos::v1::Volume::Source::CSIVolume::VolumeCapability
            ::AccessMode::SINGLE_NODE_WRITER)}));

  Future<v1::scheduler::Event::Update> startingUpdate;
  Future<v1::scheduler::Event::Update> runningUpdate;
  Future<v1::scheduler::Event::Update> finishedUpdate;

  testing::Sequence taskSequence;
  EXPECT_CALL(*scheduler, update(_, TaskStatusUpdateStateEq(v1::TASK_STARTING)))
    .InSequence(taskSequence)
    .WillOnce(DoAll(
        FutureArg<1>(&startingUpdate),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillRepeatedly(v1::scheduler::SendAcknowledge(frameworkId, agentId));

  EXPECT_CALL(*scheduler, update(_, TaskStatusUpdateStateEq(v1::TASK_RUNNING)))
    .InSequence(taskSequence)
    .WillOnce(DoAll(
        FutureArg<1>(&runningUpdate),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillRepeatedly(v1::scheduler::SendAcknowledge(frameworkId, agentId));

  EXPECT_CALL(*scheduler, update(_, TaskStatusUpdateStateEq(v1::TASK_FINISHED)))
    .InSequence(taskSequence)
    .WillOnce(DoAll(
        FutureArg<1>(&finishedUpdate),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillRepeatedly(v1::scheduler::SendAcknowledge(frameworkId, agentId));

  mesos.send(v1::createCallAccept(
      frameworkId,
      offer,
      {v1::LAUNCH({taskInfo})}));

  AWAIT_READY(startingUpdate);
  AWAIT_READY(runningUpdate);
  AWAIT_READY(finishedUpdate);
}


// When the agent fails over while a CSI volume is mounted to a container, the
// agent should recover the volume state so that the volume can be successfully
// unpublished after agent recovery is complete.
TEST_P(VolumeCSIIsolatorTest, ROOT_INTERNET_CURL_UnpublishAfterAgentFailover)
{
  createCsiPluginConfig(Bytes(0), TEST_VOLUME_ID + ":1MB");

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags agentFlags = CreateSlaveFlags();

  Fetcher fetcher(agentFlags);

  // Use a consistent ID across agent restart so that the executor can register.
  string processId = process::ID::generate("slave");

  SlaveOptions agentOptions = SlaveOptions(detector.get())
    .withId(processId)
    .withFlags(agentFlags);

  Try<Owned<cluster::Slave>> agent = StartSlave(agentOptions);
  ASSERT_SOME(agent);

  AWAIT_READY(slaveRegisteredMessage);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(frameworkInfo));

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(subscribed);
  v1::FrameworkID frameworkId(subscribed->framework_id());

  v1::Resources resources =
    v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  v1::ExecutorInfo executorInfo = v1::createExecutorInfo(
      v1::DEFAULT_EXECUTOR_ID,
      None(),
      resources,
      v1::ExecutorInfo::DEFAULT,
      frameworkId);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  v1::Offer offer = offers->offers(0);
  const v1::AgentID& agentId = offer.agent_id();

  // Run a command which will loop until a file disappears. This allows us to
  // terminate the task after agent failover.
  Try<string> taskCommand = strings::format(
      "touch %s && while [ -e %s ]; do : sleep 0.01 ; done",
      TEST_CONTAINER_PATH + TEST_OUTPUT_FILE,
      TEST_CONTAINER_PATH + TEST_OUTPUT_FILE);

  v1::TaskInfo taskInfo = v1::createTask(agentId, resources, taskCommand.get());

  taskInfo.mutable_container()->CopyFrom(v1::createContainerInfo(
      "alpine",
      {v1::createVolumeCsi(
          TEST_CSI_PLUGIN_TYPE,
          TEST_VOLUME_ID,
          TEST_CONTAINER_PATH,
          mesos::v1::Volume::RW,
          mesos::v1::Volume::Source::CSIVolume::VolumeCapability
            ::AccessMode::SINGLE_NODE_WRITER)}));

  Future<v1::scheduler::Event::Update> startingUpdate;
  Future<v1::scheduler::Event::Update> runningUpdate;
  Future<v1::scheduler::Event::Update> finishedUpdate;

  testing::Sequence taskSequence;
  EXPECT_CALL(
      *scheduler,
      update(_, TaskStatusUpdateStateEq(v1::TASK_STARTING)))
    .InSequence(taskSequence)
    .WillOnce(DoAll(
        FutureArg<1>(&startingUpdate),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillRepeatedly(v1::scheduler::SendAcknowledge(frameworkId, agentId));

  EXPECT_CALL(
      *scheduler,
      update(_, TaskStatusUpdateStateEq(v1::TASK_RUNNING)))
    .InSequence(taskSequence)
    .WillOnce(DoAll(
        FutureArg<1>(&runningUpdate),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillRepeatedly(v1::scheduler::SendAcknowledge(frameworkId, agentId));

  EXPECT_CALL(
      *scheduler,
      update(_, TaskStatusUpdateStateEq(v1::TASK_FINISHED)))
    .InSequence(taskSequence)
    .WillOnce(DoAll(
        FutureArg<1>(&finishedUpdate),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillRepeatedly(v1::scheduler::SendAcknowledge(frameworkId, agentId));

  Future<StatusUpdateAcknowledgementMessage> acknowledgement =
    FUTURE_PROTOBUF(StatusUpdateAcknowledgementMessage(), _, _);

  mesos.send(
      v1::createCallAccept(
          frameworkId,
          offer,
          {v1::LAUNCH_GROUP(
              executorInfo, v1::createTaskGroupInfo({taskInfo}))}));

  AWAIT_READY(startingUpdate);
  AWAIT_READY(acknowledgement);

  // We wait for this acknowledgement to ensure that the agent will not re-send
  // the TASK_RUNNING update after recovery.
  acknowledgement = FUTURE_PROTOBUF(StatusUpdateAcknowledgementMessage(), _, _);

  AWAIT_READY(runningUpdate);
  AWAIT_READY(acknowledgement);

  const string targetPath = csi::paths::getMountTargetPath(
      csi::paths::getMountRootDir(
          slave::paths::getCsiRootDir(agentFlags.work_dir),
          TEST_CSI_PLUGIN_TYPE,
          "default"),
      TEST_VOLUME_ID);

  ASSERT_TRUE(os::exists(targetPath));

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  // Fail over the agent and restart with a new containerizer.
  agent.get()->terminate();
  agent->reset();

  agent = StartSlave(agentOptions);
  ASSERT_SOME(agent);

  AWAIT_READY(slaveReregisteredMessage);

  // Signal the task to complete.
  ASSERT_SOME(os::rm(path::join(targetPath, TEST_OUTPUT_FILE)));

  AWAIT_READY(finishedUpdate);

  ASSERT_FALSE(os::exists(targetPath));
}


// When a task with a CSI volume finishes while the Mesos agent is down, the CSI
// volume should be correctly unpublished when the agent recovers.
TEST_P(VolumeCSIIsolatorTest, ROOT_INTERNET_CURL_FinishedWhileAgentDown)
{
  createCsiPluginConfig(Bytes(0), TEST_VOLUME_ID + ":1MB");

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags agentFlags = CreateSlaveFlags();

  Fetcher fetcher(agentFlags);

  // Use a consistent ID across agent restart so that the executor can register.
  string processId = process::ID::generate("slave");

  SlaveOptions agentOptions = SlaveOptions(detector.get())
    .withId(processId)
    .withFlags(agentFlags);

  Try<Owned<cluster::Slave>> agent = StartSlave(agentOptions);
  ASSERT_SOME(agent);

  AWAIT_READY(slaveRegisteredMessage);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(frameworkInfo));

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(subscribed);
  v1::FrameworkID frameworkId(subscribed->framework_id());

  v1::Resources resources =
    v1::Resources::parse("cpus:0.1;mem:32;disk:32").get();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  v1::Offer offer = offers->offers(0);
  const v1::AgentID& agentId = offer.agent_id();

  // Run a command which will loop until a file disappears. This allows us to
  // terminate the task after agent failover.
  Try<string> taskCommand = strings::format(
      "touch %s && while [ -e %s ]; do : sleep 0.1 ; done",
      TEST_CONTAINER_PATH + TEST_OUTPUT_FILE,
      TEST_CONTAINER_PATH + TEST_OUTPUT_FILE);

  v1::TaskInfo taskInfo = v1::createTask(agentId, resources, taskCommand.get());

  taskInfo.mutable_container()->CopyFrom(v1::createContainerInfo(
      "alpine",
      {v1::createVolumeCsi(
          TEST_CSI_PLUGIN_TYPE,
          TEST_VOLUME_ID,
          TEST_CONTAINER_PATH,
          mesos::v1::Volume::RW,
          mesos::v1::Volume::Source::CSIVolume::VolumeCapability
            ::AccessMode::SINGLE_NODE_WRITER)}));

  v1::ExecutorInfo executorInfo = v1::createExecutorInfo(
      v1::DEFAULT_EXECUTOR_ID,
      None(),
      resources,
      v1::ExecutorInfo::DEFAULT,
      frameworkId);

  Future<v1::scheduler::Event::Update> startingUpdate;
  Future<v1::scheduler::Event::Update> runningUpdate;
  Future<v1::scheduler::Event::Update> finishedUpdate;

  testing::Sequence taskSequence;
  EXPECT_CALL(
      *scheduler,
      update(_, TaskStatusUpdateStateEq(v1::TASK_STARTING)))
    .InSequence(taskSequence)
    .WillOnce(DoAll(
        FutureArg<1>(&startingUpdate),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillRepeatedly(v1::scheduler::SendAcknowledge(frameworkId, agentId));

  EXPECT_CALL(
      *scheduler,
      update(_, TaskStatusUpdateStateEq(v1::TASK_RUNNING)))
    .InSequence(taskSequence)
    .WillOnce(DoAll(
        FutureArg<1>(&runningUpdate),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillRepeatedly(v1::scheduler::SendAcknowledge(frameworkId, agentId));

  EXPECT_CALL(
      *scheduler,
      update(_, TaskStatusUpdateStateEq(v1::TASK_FINISHED)))
    .InSequence(taskSequence)
    .WillOnce(DoAll(
        FutureArg<1>(&finishedUpdate),
        v1::scheduler::SendAcknowledge(frameworkId, agentId)))
    .WillRepeatedly(v1::scheduler::SendAcknowledge(frameworkId, agentId));

  Future<StatusUpdateAcknowledgementMessage> acknowledgement =
    FUTURE_PROTOBUF(StatusUpdateAcknowledgementMessage(), _, _);

  mesos.send(v1::createCallAccept(
      frameworkId,
      offer,
      {v1::LAUNCH_GROUP(executorInfo, v1::createTaskGroupInfo({taskInfo}))}));

  AWAIT_READY(startingUpdate);
  AWAIT_READY(acknowledgement);

  // We wait for this acknowledgement to ensure that the agent will not re-send
  // the TASK_RUNNING update after recovery.
  acknowledgement = FUTURE_PROTOBUF(StatusUpdateAcknowledgementMessage(), _, _);

  AWAIT_READY(runningUpdate);
  AWAIT_READY(acknowledgement);

  const string targetPath = csi::paths::getMountTargetPath(
      csi::paths::getMountRootDir(
          slave::paths::getCsiRootDir(agentFlags.work_dir),
          TEST_CSI_PLUGIN_TYPE,
          "default"),
      TEST_VOLUME_ID);

  ASSERT_TRUE(os::exists(targetPath));

  v1::ContainerStatus status = runningUpdate->status().container_status();

  ASSERT_TRUE(status.has_container_id());

  Result<pid_t> containerPid = getContainerPid(
      agentFlags.runtime_dir,
      devolve(status.container_id()));

  ASSERT_SOME(containerPid);

  Future<Option<int>> reaped = process::reap(containerPid.get());

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  agent.get()->terminate();
  agent->reset();

  // Signal the task to complete.
  ASSERT_SOME(os::rm(path::join(targetPath, TEST_OUTPUT_FILE)));

  AWAIT_READY(reaped);

  agent = StartSlave(agentOptions);
  ASSERT_SOME(agent);

  AWAIT_READY(slaveReregisteredMessage);

  AWAIT_READY(finishedUpdate);

  ASSERT_FALSE(os::exists(targetPath));
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
