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

#ifndef __TESTS_MESOS_HPP__
#define __TESTS_MESOS_HPP__

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <gmock/gmock.h>

#include <mesos/executor.hpp>
#include <mesos/scheduler.hpp>

#include <mesos/v1/executor.hpp>
#include <mesos/v1/resources.hpp>
#include <mesos/v1/resource_provider.hpp>
#include <mesos/v1/scheduler.hpp>

#include <mesos/v1/executor/executor.hpp>

#include <mesos/v1/scheduler/scheduler.hpp>

#include <mesos/authentication/secret_generator.hpp>

#include <mesos/authorizer/authorizer.hpp>

#include <mesos/fetcher/fetcher.hpp>

#include <mesos/master/detector.hpp>

#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/http.hpp>
#include <process/io.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>
#include <process/process.hpp>
#include <process/queue.hpp>
#include <process/subprocess.hpp>

#include <process/ssl/flags.hpp>
#include <process/ssl/gtest.hpp>

#include <stout/bytes.hpp>
#include <stout/foreach.hpp>
#include <stout/gtest.hpp>
#include <stout/lambda.hpp>
#include <stout/none.hpp>
#include <stout/option.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>
#include <stout/unreachable.hpp>
#include <stout/uuid.hpp>

#include "authentication/executor/jwt_secret_generator.hpp"

#include "common/http.hpp"
#include "common/protobuf_utils.hpp"

#include "messages/messages.hpp" // For google::protobuf::Message.

#include "master/master.hpp"

#include "sched/constants.hpp"

#include "resource_provider/detector.hpp"

#include "slave/constants.hpp"
#include "slave/csi_server.hpp"
#include "slave/slave.hpp"

#include "slave/containerizer/containerizer.hpp"

#include "slave/containerizer/mesos/containerizer.hpp"

#include "tests/cluster.hpp"
#include "tests/limiter.hpp"
#include "tests/utils.hpp"

#ifdef MESOS_HAS_JAVA
#include "tests/zookeeper.hpp"
#endif // MESOS_HAS_JAVA

using ::testing::_;
using ::testing::An;
using ::testing::DoDefault;
using ::testing::Invoke;
using ::testing::Return;

namespace mesos {
namespace internal {
namespace tests {

constexpr char READONLY_HTTP_AUTHENTICATION_REALM[] = "test-readonly-realm";
constexpr char READWRITE_HTTP_AUTHENTICATION_REALM[] = "test-readwrite-realm";
constexpr char DEFAULT_TEST_ROLE[] = "default-role";
constexpr char DEFAULT_JWT_SECRET_KEY[] =
  "72kUKUFtghAjNbIOvLzfF2RxNBfeM64Bri8g9WhpyaunwqRB/yozHAqSnyHbddAV"
  "PcWRQlrJAt871oWgSH+n52vMZ3aVI+AFMzXSo8+sUfMk83IGp0WJefhzeQsjDlGH"
  "GYQgCAuGim0BE2X5U+lEue8s697uQpAO8L/FFRuDH2s";


// Forward declarations.
class MockExecutor;


struct SlaveOptions
{
  SlaveOptions(
      mesos::master::detector::MasterDetector* detector,
      bool mock = false)
    : detector(detector), mock(mock)
  {}

  SlaveOptions& withFlags(const Option<slave::Flags>& flags)
  {
    this->flags = flags;
    return *this;
  }

  SlaveOptions& withId(const Option<std::string>& id)
  {
    this->id = id;
    return *this;
  }

  SlaveOptions& withContainerizer(
      const Option<slave::Containerizer*>& containerizer)
  {
    this->containerizer = containerizer;
    return *this;
  }

  SlaveOptions& withGc(const Option<slave::GarbageCollector*>& gc)
  {
    this->gc = gc;
    return *this;
  }

  SlaveOptions& withTaskStatusUpdateManager(
      const Option<slave::TaskStatusUpdateManager*>& taskStatusUpdateManager)
  {
    this->taskStatusUpdateManager = taskStatusUpdateManager;
    return *this;
  }

  SlaveOptions& withResourceEstimator(
      const Option<mesos::slave::ResourceEstimator*>& resourceEstimator)
  {
    this->resourceEstimator = resourceEstimator;
    return *this;
  }

  SlaveOptions& withQosController(
      const Option<mesos::slave::QoSController*>& qosController)
  {
    this->qosController = qosController;
    return *this;
  }

  SlaveOptions& withSecretGenerator(
      const Option<mesos::SecretGenerator*>& secretGenerator)
  {
    this->secretGenerator = secretGenerator;
    return *this;
  }

  SlaveOptions& withAuthorizer(const Option<Authorizer*>& authorizer)
  {
    this->authorizer = authorizer;
    return *this;
  }

  SlaveOptions& withFutureTracker(
      const Option<PendingFutureTracker*>& futureTracker)
  {
    this->futureTracker = futureTracker;
    return *this;
  }

  SlaveOptions& withCsiServer(const process::Owned<slave::CSIServer>& csiServer)
  {
    this->csiServer = csiServer;
    return *this;
  }

  mesos::master::detector::MasterDetector* detector;
  bool mock;
  Option<slave::Flags> flags;
  Option<std::string> id;
  Option<slave::Containerizer*> containerizer;
  Option<slave::GarbageCollector*> gc;
  Option<slave::TaskStatusUpdateManager*> taskStatusUpdateManager;
  Option<mesos::slave::ResourceEstimator*> resourceEstimator;
  Option<mesos::slave::QoSController*> qosController;
  Option<mesos::SecretGenerator*> secretGenerator;
  Option<Authorizer*> authorizer;
  Option<PendingFutureTracker*> futureTracker;
  Option<process::Owned<slave::CSIServer>> csiServer;
};


// NOTE: `SSLTemporaryDirectoryTest` exists even when SSL is not compiled into
// Mesos.  In this case, the class is an alias of `TemporaryDirectoryTest`.
class MesosTest : public SSLTemporaryDirectoryTest
{
public:
  static void SetUpTestCase();
  static void TearDownTestCase();

protected:
  MesosTest(const Option<zookeeper::URL>& url = None());

  // Returns the flags used to create masters.
  virtual master::Flags CreateMasterFlags();

  // Returns the flags used to create slaves.
  virtual slave::Flags CreateSlaveFlags();

  // Starts a master with the specified flags.
  virtual Try<process::Owned<cluster::Master>> StartMaster(
      const Option<master::Flags>& flags = None());

  // Starts a master with the specified allocator process and flags.
  virtual Try<process::Owned<cluster::Master>> StartMaster(
      mesos::allocator::Allocator* allocator,
      const Option<master::Flags>& flags = None());

  // Starts a master with the specified authorizer and flags.
  virtual Try<process::Owned<cluster::Master>> StartMaster(
      Authorizer* authorizer,
      const Option<master::Flags>& flags = None());

  // Starts a master with a slave removal rate limiter and flags.
  // NOTE: The `slaveRemovalLimiter` is a `shared_ptr` because the
  // underlying `Master` process requires the pointer in this form.
  virtual Try<process::Owned<cluster::Master>> StartMaster(
      const std::shared_ptr<MockRateLimiter>& slaveRemovalLimiter,
      const Option<master::Flags>& flags = None());

  // Starts a slave with the specified options.
  // NOTE: This is a preferred method to start a slave.
  // The other overloads of `StartSlave` are DEPRECATED!
  virtual Try<process::Owned<cluster::Slave>> StartSlave(
      const SlaveOptions& options);

  // Starts a slave with the specified detector and flags.
  virtual Try<process::Owned<cluster::Slave>> StartSlave(
      mesos::master::detector::MasterDetector* detector,
      const Option<slave::Flags>& flags = None(),
      bool mock = false);

  // Starts a slave with the specified detector, containerizer, and flags.
  virtual Try<process::Owned<cluster::Slave>> StartSlave(
      mesos::master::detector::MasterDetector* detector,
      slave::Containerizer* containerizer,
      const Option<slave::Flags>& flags = None(),
      bool mock = false);

  // Starts a slave with the specified detector, id, and flags.
  virtual Try<process::Owned<cluster::Slave>> StartSlave(
      mesos::master::detector::MasterDetector* detector,
      const std::string& id,
      const Option<slave::Flags>& flags = None(),
      bool mock = false);

  // Starts a slave with the specified detector, containerizer, id, and flags.
  virtual Try<process::Owned<cluster::Slave>> StartSlave(
      mesos::master::detector::MasterDetector* detector,
      slave::Containerizer* containerizer,
      const std::string& id,
      const Option<slave::Flags>& flags = None());

  // Starts a slave with the specified detector, GC, and flags.
  virtual Try<process::Owned<cluster::Slave>> StartSlave(
      mesos::master::detector::MasterDetector* detector,
      slave::GarbageCollector* gc,
      const Option<slave::Flags>& flags = None(),
      bool mock = false);

  // Starts a slave with the specified detector, resource estimator, and flags.
  virtual Try<process::Owned<cluster::Slave>> StartSlave(
      mesos::master::detector::MasterDetector* detector,
      mesos::slave::ResourceEstimator* resourceEstimator,
      const Option<slave::Flags>& flags = None());

  // Starts a slave with the specified detector, containerizer,
  // resource estimator, and flags.
  virtual Try<process::Owned<cluster::Slave>> StartSlave(
      mesos::master::detector::MasterDetector* detector,
      slave::Containerizer* containerizer,
      mesos::slave::ResourceEstimator* resourceEstimator,
      const Option<slave::Flags>& flags = None());

  // Starts a slave with the specified detector, QoS Controller, and flags.
  virtual Try<process::Owned<cluster::Slave>> StartSlave(
      mesos::master::detector::MasterDetector* detector,
      mesos::slave::QoSController* qosController,
      const Option<slave::Flags>& flags = None());

  // Starts a slave with the specified detector, containerizer,
  // QoS Controller, and flags.
  virtual Try<process::Owned<cluster::Slave>> StartSlave(
      mesos::master::detector::MasterDetector* detector,
      slave::Containerizer* containerizer,
      mesos::slave::QoSController* qosController,
      const Option<slave::Flags>& flags = None(),
      bool mock = false);

  // Starts a slave with the specified detector, authorizer, and flags.
  virtual Try<process::Owned<cluster::Slave>> StartSlave(
      mesos::master::detector::MasterDetector* detector,
      mesos::Authorizer* authorizer,
      const Option<slave::Flags>& flags = None(),
      bool mock = false);

  // Starts a slave with the specified detector, containerizer, authorizer,
  // and flags.
  virtual Try<process::Owned<cluster::Slave>> StartSlave(
      mesos::master::detector::MasterDetector* detector,
      slave::Containerizer* containerizer,
      mesos::Authorizer* authorizer,
      const Option<slave::Flags>& flags = None(),
      bool mock = false);

  // Starts a slave with the specified detector, containerizer,
  // secretGenerator, authorizer and flags.
  virtual Try<process::Owned<cluster::Slave>> StartSlave(
      mesos::master::detector::MasterDetector* detector,
      slave::Containerizer* containerizer,
      mesos::SecretGenerator* secretGenerator,
      const Option<mesos::Authorizer*>& authorizer = None(),
      const Option<slave::Flags>& flags = None(),
      bool mock = false);

  // Starts a slave with the specified detector, secretGenerator,
  // and flags.
  virtual Try<process::Owned<cluster::Slave>> StartSlave(
      mesos::master::detector::MasterDetector* detector,
      mesos::SecretGenerator* secretGenerator,
      const Option<slave::Flags>& flags = None());

  Option<zookeeper::URL> zookeeperUrl;

  // NOTE: On Windows, most tasks are run under PowerShell, which uses ~150 MB
  // of memory per-instance due to loading .NET. Realistically, PowerShell can
  // be called more than once in a task, so 512 MB is the safe minimum.
  // Furthermore, because the Windows `cpu` isolator is a hard-cap, 0.1 CPUs
  // will cause the task (or even a check command) to timeout, so 1 CPU is the
  // safe minimum.
  //
  // Because multiple tasks can be run, the default agent resources needs to be
  // at least a multiple of the default task resources: four times seems safe.
  //
  // On platforms where the shell is, e.g. Bash, the minimum is much lower.
  const std::string defaultAgentResourcesString{
#ifdef __WINDOWS__
      "cpus:4;gpus:0;mem:2048;disk:1024;ports:[31000-32000]"
#else
      "cpus:2;gpus:0;mem:1024;disk:1024;ports:[31000-32000]"
#endif // __WINDOWS__
      };

  const std::string defaultTaskResourcesString{
#ifdef __WINDOWS__
      "cpus:1;mem:512;disk:32"
#else
      "cpus:0.1;mem:32;disk:32"
#endif // __WINDOWS__
      };
};


template <typename T>
class ContainerizerTest : public MesosTest {};

#ifdef __linux__
// Cgroups hierarchy used by the cgroups related tests.
const static std::string TEST_CGROUPS_HIERARCHY = "/tmp/mesos_test_cgroup";

// Name of the root cgroup used by the cgroups related tests.
const static std::string TEST_CGROUPS_ROOT = "mesos_test";


template <>
class ContainerizerTest<slave::MesosContainerizer> : public MesosTest
{
public:
  static void SetUpTestCase();
  static void TearDownTestCase();

protected:
  slave::Flags CreateSlaveFlags() override;
  void SetUp() override;
  void TearDown() override;

private:
  // Base hierarchy for separately mounted cgroup controllers, e.g., if the
  // base hierarchy is /sys/fs/cgroup then each controller will be mounted to
  // /sys/fs/cgroup/{controller}/.
  std::string baseHierarchy;

  // Set of cgroup subsystems used by the cgroups related tests.
  hashset<std::string> subsystems;
};
#else
template <>
class ContainerizerTest<slave::MesosContainerizer> : public MesosTest
{
protected:
  virtual slave::Flags CreateSlaveFlags();
};
#endif // __linux__


#ifdef MESOS_HAS_JAVA

class MesosZooKeeperTest : public MesosTest
{
public:
  static void SetUpTestCase()
  {
    // Make sure the JVM is created.
    ZooKeeperTest::SetUpTestCase();

    // Launch the ZooKeeper test server.
    server = new ZooKeeperTestServer();
    server->startNetwork();

    Try<zookeeper::URL> parse = zookeeper::URL::parse(
        "zk://" + server->connectString() + "/znode");
    ASSERT_SOME(parse);

    url = parse.get();
  }

  static void TearDownTestCase()
  {
    delete server;
    server = nullptr;
  }

  void SetUp() override
  {
    MesosTest::SetUp();
    server->startNetwork();
  }

  void TearDown() override
  {
    server->shutdownNetwork();
    MesosTest::TearDown();
  }

protected:
  MesosZooKeeperTest() : MesosTest(url) {}

  master::Flags CreateMasterFlags() override
  {
    master::Flags flags = MesosTest::CreateMasterFlags();

    // NOTE: Since we are using the replicated log with ZooKeeper
    // (default storage in MesosTest), we need to specify the quorum.
    flags.quorum = 1;

    return flags;
  }

  static ZooKeeperTestServer* server;
  static Option<zookeeper::URL> url;
};

#endif // MESOS_HAS_JAVA

namespace v1 {

// Alias existing `mesos::v1` namespaces so that we can easily write
// `v1::` in tests.
//
// TODO(jmlvanre): Remove these aliases once we clean up the `tests`
// namespace hierarchy.
namespace agent = mesos::v1::agent;
namespace maintenance = mesos::v1::maintenance;
namespace master = mesos::v1::master;
namespace quota = mesos::v1::quota;

using mesos::v1::OPERATION_PENDING;
using mesos::v1::OPERATION_FINISHED;
using mesos::v1::OPERATION_FAILED;
using mesos::v1::OPERATION_ERROR;
using mesos::v1::OPERATION_DROPPED;
using mesos::v1::OPERATION_UNREACHABLE;
using mesos::v1::OPERATION_GONE_BY_OPERATOR;
using mesos::v1::OPERATION_RECOVERING;
using mesos::v1::OPERATION_UNKNOWN;

using mesos::v1::TASK_STAGING;
using mesos::v1::TASK_STARTING;
using mesos::v1::TASK_RUNNING;
using mesos::v1::TASK_KILLING;
using mesos::v1::TASK_FINISHED;
using mesos::v1::TASK_FAILED;
using mesos::v1::TASK_KILLED;
using mesos::v1::TASK_ERROR;
using mesos::v1::TASK_LOST;
using mesos::v1::TASK_DROPPED;
using mesos::v1::TASK_UNREACHABLE;
using mesos::v1::TASK_GONE;
using mesos::v1::TASK_GONE_BY_OPERATOR;
using mesos::v1::TASK_UNKNOWN;

using mesos::v1::AgentID;
using mesos::v1::CheckInfo;
using mesos::v1::CommandInfo;
using mesos::v1::ContainerID;
using mesos::v1::ContainerStatus;
using mesos::v1::Environment;
using mesos::v1::ExecutorID;
using mesos::v1::ExecutorInfo;
using mesos::v1::Filters;
using mesos::v1::FrameworkID;
using mesos::v1::FrameworkInfo;
using mesos::v1::HealthCheck;
using mesos::v1::InverseOffer;
using mesos::v1::MachineID;
using mesos::v1::Metric;
using mesos::v1::Offer;
using mesos::v1::OperationID;
using mesos::v1::OperationState;
using mesos::v1::OperationStatus;
using mesos::v1::Resource;
using mesos::v1::ResourceProviderID;
using mesos::v1::ResourceProviderInfo;
using mesos::v1::Resources;
using mesos::v1::TaskID;
using mesos::v1::TaskInfo;
using mesos::v1::TaskGroupInfo;
using mesos::v1::TaskState;
using mesos::v1::TaskStatus;
using mesos::v1::UUID;
using mesos::v1::WeightInfo;

} // namespace v1 {

namespace common {

template <typename TCredential>
struct DefaultCredential
{
  static TCredential create()
  {
    TCredential credential;
    credential.set_principal("test-principal");
    credential.set_secret("test-secret");
    return credential;
  }
};


// TODO(jmlvanre): consider factoring this out.
template <typename TCredential>
struct DefaultCredential2
{
  static TCredential create()
  {
    TCredential credential;
    credential.set_principal("test-principal-2");
    credential.set_secret("test-secret-2");
    return credential;
  }
};


template <typename TFrameworkInfo, typename TCredential>
struct DefaultFrameworkInfo
{
  static TFrameworkInfo create()
  {
    TFrameworkInfo framework;
    framework.set_name("default");
    framework.set_user(os::user().get());
    framework.set_principal(
        DefaultCredential<TCredential>::create().principal());
    framework.add_roles("*");
    framework.add_capabilities()->set_type(
        TFrameworkInfo::Capability::MULTI_ROLE);
    framework.add_capabilities()->set_type(
        TFrameworkInfo::Capability::RESERVATION_REFINEMENT);

    return framework;
  }
};

} // namespace common {

// TODO(jmlvanre): Remove `inline` once we have adjusted all tests to
// distinguish between `internal` and `v1`.
inline namespace internal {
using DefaultCredential = common::DefaultCredential<Credential>;
using DefaultCredential2 = common::DefaultCredential2<Credential>;
using DefaultFrameworkInfo =
  common::DefaultFrameworkInfo<FrameworkInfo, Credential>;
}  // namespace internal {


namespace v1 {
using DefaultCredential = common::DefaultCredential<mesos::v1::Credential>;
using DefaultCredential2 = common::DefaultCredential2<mesos::v1::Credential>;
using DefaultFrameworkInfo =
  common::DefaultFrameworkInfo<mesos::v1::FrameworkInfo, mesos::v1::Credential>;
}  // namespace v1 {


// We factor out all common behavior and templatize it so that we can
// can call it from both `v1::` and `internal::`.
namespace common {

template <typename TCommandInfo>
inline TCommandInfo createCommandInfo(
    const Option<std::string>& value = None(),
    const std::vector<std::string>& arguments = {})
{
  TCommandInfo commandInfo;
  if (value.isSome()) {
    commandInfo.set_value(value.get());
  }
  if (!arguments.empty()) {
    commandInfo.set_shell(false);
    foreach (const std::string& arg, arguments) {
      commandInfo.add_arguments(arg);
    }
  }
  return commandInfo;
}


template <typename TExecutorInfo,
          typename TExecutorID,
          typename TResources,
          typename TCommandInfo,
          typename TFrameworkID>
inline TExecutorInfo createExecutorInfo(
    const TExecutorID& executorId,
    const Option<TCommandInfo>& command,
    const Option<TResources>& resources,
    const Option<typename TExecutorInfo::Type>& type,
    const Option<TFrameworkID>& frameworkId)
{
  TExecutorInfo executor;
  executor.mutable_executor_id()->CopyFrom(executorId);
  if (command.isSome()) {
    executor.mutable_command()->CopyFrom(command.get());
  }
  if (resources.isSome()) {
    executor.mutable_resources()->CopyFrom(resources.get());
  }
  if (type.isSome()) {
    executor.set_type(type.get());
  }
  if (frameworkId.isSome()) {
    executor.mutable_framework_id()->CopyFrom(frameworkId.get());
  }
  return executor;
}


template <typename TExecutorInfo,
          typename TExecutorID,
          typename TResources,
          typename TCommandInfo,
          typename TFrameworkID>
inline TExecutorInfo createExecutorInfo(
    const std::string& _executorId,
    const Option<TCommandInfo>& command,
    const Option<TResources>& resources,
    const Option<typename TExecutorInfo::Type>& type,
    const Option<TFrameworkID>& frameworkId)
{
  TExecutorID executorId;
  executorId.set_value(_executorId);
  return createExecutorInfo<TExecutorInfo,
                            TExecutorID,
                            TResources,
                            TCommandInfo,
                            TFrameworkID>(
      executorId, command, resources, type, frameworkId);
}


template <typename TExecutorInfo,
          typename TExecutorID,
          typename TResources,
          typename TCommandInfo,
          typename TFrameworkID>
inline TExecutorInfo createExecutorInfo(
    const std::string& executorId,
    const Option<TCommandInfo>& command = None(),
    const Option<std::string>& resources = None(),
    const Option<typename TExecutorInfo::Type>& type = None(),
    const Option<TFrameworkID>& frameworkId = None())
{
  if (resources.isSome()) {
    return createExecutorInfo<TExecutorInfo,
                              TExecutorID,
                              TResources,
                              TCommandInfo,
                              TFrameworkID>(
        executorId,
        command,
        TResources::parse(resources.get()).get(),
        type,
        frameworkId);
  }

  return createExecutorInfo<TExecutorInfo,
                            TExecutorID,
                            TResources,
                            TCommandInfo,
                            TFrameworkID>(
      executorId, command, Option<TResources>::none(), type, frameworkId);
}


template <typename TExecutorInfo,
          typename TExecutorID,
          typename TResources,
          typename TCommandInfo,
          typename TFrameworkID>
inline TExecutorInfo createExecutorInfo(
    const TExecutorID& executorId,
    const Option<TCommandInfo>& command,
    const std::string& resources,
    const Option<typename TExecutorInfo::Type>& type = None(),
    const Option<TFrameworkID>& frameworkId = None())
{
  return createExecutorInfo<TExecutorInfo,
                            TExecutorID,
                            TResources,
                            TCommandInfo,
                            TFrameworkID>(
      executorId,
      command,
      TResources::parse(resources).get(),
      type,
      frameworkId);
}


template <typename TExecutorInfo,
          typename TExecutorID,
          typename TResources,
          typename TCommandInfo,
          typename TFrameworkID>
inline TExecutorInfo createExecutorInfo(
    const std::string& executorId,
    const std::string& command,
    const Option<std::string>& resources = None(),
    const Option<typename TExecutorInfo::Type>& type = None(),
    const Option<TFrameworkID>& frameworkId = None())
{
  TCommandInfo commandInfo = createCommandInfo<TCommandInfo>(command);
  return createExecutorInfo<TExecutorInfo,
                            TExecutorID,
                            TResources,
                            TCommandInfo,
                            TFrameworkID>(
      executorId, commandInfo, resources, type, frameworkId);
}


template <typename TImage>
inline TImage createDockerImage(const std::string& imageName)
{
  TImage image;
  image.set_type(TImage::DOCKER);
  image.mutable_docker()->set_name(imageName);
  return image;
}


template <typename TVolume>
inline TVolume createVolumeSandboxPath(
    const std::string& containerPath,
    const std::string& sandboxPath,
    const typename TVolume::Mode& mode)
{
  TVolume volume;
  volume.set_container_path(containerPath);
  volume.set_mode(mode);

  // TODO(jieyu): Use TVolume::Source::SANDBOX_PATH.
  volume.set_host_path(sandboxPath);

  return volume;
}


template <typename TVolume, typename TMountPropagation>
inline TVolume createVolumeHostPath(
    const std::string& containerPath,
    const std::string& hostPath,
    const typename TVolume::Mode& mode,
    const Option<typename TMountPropagation::Mode>& mountPropagationMode =
      None())
{
  TVolume volume;
  volume.set_container_path(containerPath);
  volume.set_mode(mode);

  typename TVolume::Source* source = volume.mutable_source();
  source->set_type(TVolume::Source::HOST_PATH);
  source->mutable_host_path()->set_path(hostPath);

  if (mountPropagationMode.isSome()) {
    source
      ->mutable_host_path()
      ->mutable_mount_propagation()
      ->set_mode(mountPropagationMode.get());
  }

  return volume;
}


template <typename TVolume, typename TImage>
inline TVolume createVolumeFromDockerImage(
    const std::string& containerPath,
    const std::string& imageName,
    const typename TVolume::Mode& mode)
{
  TVolume volume;
  volume.set_container_path(containerPath);
  volume.set_mode(mode);
  volume.mutable_image()->CopyFrom(createDockerImage<TImage>(imageName));
  return volume;
}


template <typename TVolume>
inline TVolume createVolumeCsi(
    const std::string& pluginName,
    const std::string& volumeId,
    const std::string& containerPath,
    const typename TVolume::Mode& mode,
    const typename TVolume::Source::CSIVolume::VolumeCapability
      ::AccessMode::Mode& accessMode)
{
  TVolume volume;
  volume.set_container_path(containerPath);
  volume.set_mode(mode);

  typename TVolume::Source* source = volume.mutable_source();
  source->set_type(TVolume::Source::CSI_VOLUME);
  source->mutable_csi_volume()->set_plugin_name(pluginName);

  typename TVolume::Source::CSIVolume::StaticProvisioning* staticInfo =
    source->mutable_csi_volume()->mutable_static_provisioning();

  staticInfo->set_volume_id(volumeId);
  staticInfo->mutable_volume_capability()->mutable_mount();
  staticInfo->mutable_volume_capability()
    ->mutable_access_mode()->set_mode(accessMode);

  return volume;
}


template <typename TNetworkInfo>
inline TNetworkInfo createNetworkInfo(
    const std::string& networkName)
{
  TNetworkInfo info;
  info.set_name(networkName);
  return info;
}


template <typename TContainerInfo, typename TVolume, typename TImage>
inline TContainerInfo createContainerInfo(
    const Option<std::string>& imageName = None(),
    const std::vector<TVolume>& volumes = {})
{
  TContainerInfo info;
  info.set_type(TContainerInfo::MESOS);

  if (imageName.isSome()) {
    TImage* image = info.mutable_mesos()->mutable_image();
    image->CopyFrom(createDockerImage<TImage>(imageName.get()));
  }

  foreach (const TVolume& volume, volumes) {
    info.add_volumes()->CopyFrom(volume);
  }

  return info;
}


inline SlaveID getAgentID(const Offer& offer)
{
  return offer.slave_id();
}


inline mesos::v1::AgentID getAgentID(const mesos::v1::Offer& offer)
{
  return offer.agent_id();
}


inline void setAgentID(TaskInfo* task, const SlaveID& slaveId)
{
  task->mutable_slave_id()->CopyFrom(slaveId);
}


inline void setAgentID(
    mesos::v1::TaskInfo* task,
    const mesos::v1::AgentID& agentId)
{
  task->mutable_agent_id()->CopyFrom(agentId);
}


// TODO(bmahler): Refactor this to make the distinction between
// command tasks and executor tasks clearer.
template <
    typename TTaskInfo,
    typename TExecutorID,
    typename TSlaveID,
    typename TResources,
    typename TExecutorInfo,
    typename TCommandInfo,
    typename TOffer,
    typename TScalar>
inline TTaskInfo createTask(
    const TSlaveID& slaveId,
    const TResources& resourceRequests,
    const TCommandInfo& command,
    const Option<TExecutorID>& executorId = None(),
    const std::string& name = "test-task",
    const std::string& id = id::UUID::random().toString(),
    const google::protobuf::Map<std::string, TScalar>& resourceLimits = {})
{
  TTaskInfo task;
  task.set_name(name);
  task.mutable_task_id()->set_value(id);
  setAgentID(&task, slaveId);
  task.mutable_resources()->CopyFrom(resourceRequests);
  if (!resourceLimits.empty()) {
    *task.mutable_limits() = resourceLimits;
  }
  if (executorId.isSome()) {
    TExecutorInfo executor;
    executor.mutable_executor_id()->CopyFrom(executorId.get());
    executor.mutable_command()->CopyFrom(command);
    task.mutable_executor()->CopyFrom(executor);
  } else {
    task.mutable_command()->CopyFrom(command);
  }

  return task;
}


template <
    typename TTaskInfo,
    typename TExecutorID,
    typename TSlaveID,
    typename TResources,
    typename TExecutorInfo,
    typename TCommandInfo,
    typename TOffer,
    typename TScalar>
inline TTaskInfo createTask(
    const TSlaveID& slaveId,
    const TResources& resourceRequests,
    const std::string& command,
    const Option<TExecutorID>& executorId = None(),
    const std::string& name = "test-task",
    const std::string& id = id::UUID::random().toString(),
    const google::protobuf::Map<std::string, TScalar>& resourceLimits = {})
{
  return createTask<
      TTaskInfo,
      TExecutorID,
      TSlaveID,
      TResources,
      TExecutorInfo,
      TCommandInfo,
      TOffer,
      TScalar>(
          slaveId,
          resourceRequests,
          createCommandInfo<TCommandInfo>(command),
          executorId,
          name,
          id,
          resourceLimits);
}


template <
    typename TTaskInfo,
    typename TExecutorID,
    typename TSlaveID,
    typename TResources,
    typename TExecutorInfo,
    typename TCommandInfo,
    typename TOffer,
    typename TScalar>
inline TTaskInfo createTask(
    const TOffer& offer,
    const std::string& command,
    const Option<TExecutorID>& executorId = None(),
    const std::string& name = "test-task",
    const std::string& id = id::UUID::random().toString(),
    const google::protobuf::Map<std::string, TScalar>& resourceLimits = {})
{
  return createTask<
      TTaskInfo,
      TExecutorID,
      TSlaveID,
      TResources,
      TExecutorInfo,
      TCommandInfo,
      TOffer,
      TScalar>(
          getAgentID(offer),
          offer.resources(),
          command,
          executorId,
          name,
          id,
          resourceLimits);
}


template <typename TTaskGroupInfo, typename TTaskInfo>
inline TTaskGroupInfo createTaskGroupInfo(const std::vector<TTaskInfo>& tasks)
{
  TTaskGroupInfo taskGroup;
  foreach (const TTaskInfo& task, tasks) {
    taskGroup.add_tasks()->CopyFrom(task);
  }
  return taskGroup;
}


template <typename TResource>
inline typename TResource::ReservationInfo createStaticReservationInfo(
    const std::string& role)
{
  typename TResource::ReservationInfo info;
  info.set_type(TResource::ReservationInfo::STATIC);
  info.set_role(role);
  return info;
}


template <typename TResource, typename TLabels>
inline typename TResource::ReservationInfo createDynamicReservationInfo(
    const std::string& role,
    const Option<std::string>& principal = None(),
    const Option<TLabels>& labels = None())
{
  typename TResource::ReservationInfo info;

  info.set_type(TResource::ReservationInfo::DYNAMIC);
  info.set_role(role);

  if (principal.isSome()) {
    info.set_principal(principal.get());
  }

  if (labels.isSome()) {
    info.mutable_labels()->CopyFrom(labels.get());
  }

  return info;
}


template <
    typename TResource,
    typename TResources,
    typename... TReservationInfos>
inline TResource createReservedResource(
    const std::string& name,
    const std::string& value,
    const TReservationInfos&... reservations)
{
  std::initializer_list<typename TResource::ReservationInfo> reservations_ = {
    reservations...
  };

  TResource resource = TResources::parse(name, value, "*").get();
  resource.mutable_reservations()->CopyFrom(
      google::protobuf::RepeatedPtrField<typename TResource::ReservationInfo>{
        reservations_.begin(), reservations_.end()});

  return resource;
}


// NOTE: We only set the volume in DiskInfo if 'containerPath' is set.
// If volume mode is not specified, Volume::RW will be used (assuming
// 'containerPath' is set).
template <typename TResource, typename TVolume>
inline typename TResource::DiskInfo createDiskInfo(
    const Option<std::string>& persistenceId,
    const Option<std::string>& containerPath,
    const Option<typename TVolume::Mode>& mode = None(),
    const Option<std::string>& hostPath = None(),
    const Option<typename TResource::DiskInfo::Source>& source = None(),
    const Option<std::string>& principal = None())
{
  typename TResource::DiskInfo info;

  if (persistenceId.isSome()) {
    info.mutable_persistence()->set_id(persistenceId.get());
  }

  if (principal.isSome()) {
    info.mutable_persistence()->set_principal(principal.get());
  }

  if (containerPath.isSome()) {
    TVolume volume;
    volume.set_container_path(containerPath.get());
    volume.set_mode(mode.isSome() ? mode.get() : TVolume::RW);

    if (hostPath.isSome()) {
      volume.set_host_path(hostPath.get());
    }

    info.mutable_volume()->CopyFrom(volume);
  }

  if (source.isSome()) {
    info.mutable_source()->CopyFrom(source.get());
  }

  return info;
}


// Helper for creating a disk source with type `PATH`.
template <typename TResource>
inline typename TResource::DiskInfo::Source createDiskSourcePath(
    const Option<std::string>& root = None(),
    const Option<std::string>& id = None(),
    const Option<std::string>& profile = None())
{
  typename TResource::DiskInfo::Source source;

  source.set_type(TResource::DiskInfo::Source::PATH);

  if (root.isSome()) {
    source.mutable_path()->set_root(root.get());
  }

  if (id.isSome()) {
    source.set_id(id.get());
  }

  if (profile.isSome()) {
    source.set_profile(profile.get());
  }

  return source;
}


// Helper for creating a disk source with type `MOUNT`.
template <typename TResource>
inline typename TResource::DiskInfo::Source createDiskSourceMount(
    const Option<std::string>& root = None(),
    const Option<std::string>& id = None(),
    const Option<std::string>& profile = None())
{
  typename TResource::DiskInfo::Source source;

  source.set_type(TResource::DiskInfo::Source::MOUNT);

  if (root.isSome()) {
    source.mutable_mount()->set_root(root.get());
  }

  if (id.isSome()) {
    source.set_id(id.get());
  }

  if (profile.isSome()) {
    source.set_profile(profile.get());
  }

  return source;
}


// Helper for creating a disk source with type `BLOCK'
template <typename TResource>
inline typename TResource::DiskInfo::Source createDiskSourceBlock(
    const Option<std::string>& id = None(),
    const Option<std::string>& profile = None())
{
  typename TResource::DiskInfo::Source source;

  source.set_type(TResource::DiskInfo::Source::BLOCK);

  if (id.isSome()) {
    source.set_id(id.get());
  }

  if (profile.isSome()) {
    source.set_profile(profile.get());
  }

  return source;
}


// Helper for creating a disk source with type `RAW'.
template <typename TResource>
inline typename TResource::DiskInfo::Source createDiskSourceRaw(
    const Option<std::string>& id = None(),
    const Option<std::string>& profile = None())
{
  typename TResource::DiskInfo::Source source;

  source.set_type(TResource::DiskInfo::Source::RAW);

  if (id.isSome()) {
    source.set_id(id.get());
  }

  if (profile.isSome()) {
    source.set_profile(profile.get());
  }

  return source;
}


// Helper for creating a disk resource.
template <typename TResource, typename TResources, typename TVolume>
inline TResource createDiskResource(
    const std::string& value,
    const std::string& role,
    const Option<std::string>& persistenceID,
    const Option<std::string>& containerPath,
    const Option<typename TResource::DiskInfo::Source>& source = None(),
    bool isShared = false)
{
  TResource resource = TResources::parse("disk", value, role).get();

  if (persistenceID.isSome() || containerPath.isSome() || source.isSome()) {
    resource.mutable_disk()->CopyFrom(
        createDiskInfo<TResource, TVolume>(
            persistenceID,
            containerPath,
            None(),
            None(),
            source));

    if (isShared) {
      resource.mutable_shared();
    }
  }

  return resource;
}


// Note that `reservationPrincipal` should be specified if and only if
// the volume uses dynamically reserved resources.
template <typename TResource, typename TResources, typename TVolume>
inline TResource createPersistentVolume(
    const Bytes& size,
    const std::string& role,
    const std::string& persistenceId,
    const std::string& containerPath,
    const Option<std::string>& reservationPrincipal = None(),
    const Option<typename TResource::DiskInfo::Source>& source = None(),
    const Option<std::string>& creatorPrincipal = None(),
    bool isShared = false)
{
  TResource volume = TResources::parse(
      "disk",
      stringify((double) size.bytes() / Bytes::MEGABYTES),
      role).get();

  volume.mutable_disk()->CopyFrom(
      createDiskInfo<TResource, TVolume>(
          persistenceId,
          containerPath,
          None(),
          None(),
          source,
          creatorPrincipal));

  if (reservationPrincipal.isSome()) {
    typename TResource::ReservationInfo& reservation =
      *volume.mutable_reservations()->rbegin();

    reservation.set_type(TResource::ReservationInfo::DYNAMIC);
    reservation.set_principal(reservationPrincipal.get());
  }

  if (isShared) {
    volume.mutable_shared();
  }

  return volume;
}


// Note that `reservationPrincipal` should be specified if and only if
// the volume uses dynamically reserved resources.
template <typename TResource, typename TResources, typename TVolume>
inline TResource createPersistentVolume(
    TResource volume,
    const std::string& persistenceId,
    const std::string& containerPath,
    const Option<std::string>& reservationPrincipal = None(),
    const Option<std::string>& creatorPrincipal = None(),
    bool isShared = false)
{
  Option<typename TResource::DiskInfo::Source> source = None();
  if (volume.has_disk() && volume.disk().has_source()) {
    source = volume.disk().source();
  }

  volume.mutable_disk()->CopyFrom(
      createDiskInfo<TResource, TVolume>(
          persistenceId,
          containerPath,
          None(),
          None(),
          source,
          creatorPrincipal));

  if (reservationPrincipal.isSome()) {
    typename TResource::ReservationInfo& reservation =
      *volume.mutable_reservations()->rbegin();

    reservation.set_type(TResource::ReservationInfo::DYNAMIC);
    reservation.set_principal(reservationPrincipal.get());
  }

  if (isShared) {
    volume.mutable_shared();
  }

  return volume;
}


template <typename TCredential>
inline process::http::Headers createBasicAuthHeaders(
    const TCredential& credential)
{
  return process::http::Headers({{
      "Authorization",
      "Basic " +
        base64::encode(credential.principal() + ":" + credential.secret())
  }});
}


// Create WeightInfos from the specified weights flag.
template <typename TWeightInfo>
inline google::protobuf::RepeatedPtrField<TWeightInfo> createWeightInfos(
    const std::string& weightsFlag)
{
  google::protobuf::RepeatedPtrField<TWeightInfo> infos;
  std::vector<std::string> tokens = strings::tokenize(weightsFlag, ",");
  foreach (const std::string& token, tokens) {
    std::vector<std::string> pair = strings::tokenize(token, "=");
    EXPECT_EQ(2u, pair.size());
    double weight = atof(pair[1].c_str());
    TWeightInfo weightInfo;
    weightInfo.set_role(pair[0]);
    weightInfo.set_weight(weight);
    infos.Add()->CopyFrom(weightInfo);
  }

  return infos;
}


// Convert WeightInfos protobuf to weights hashmap.
template <typename TWeightInfo>
inline hashmap<std::string, double> convertToHashmap(
    const google::protobuf::RepeatedPtrField<TWeightInfo> weightInfos)
{
  hashmap<std::string, double> weights;

  foreach (const TWeightInfo& weightInfo, weightInfos) {
    weights[weightInfo.role()] = weightInfo.weight();
  }

  return weights;
}


// Helper to create DomainInfo.
template <typename TDomainInfo>
inline TDomainInfo createDomainInfo(
    const std::string& regionName,
    const std::string& zoneName)
{
  TDomainInfo domain;

  domain.mutable_fault_domain()->mutable_region()->set_name(regionName);
  domain.mutable_fault_domain()->mutable_zone()->set_name(zoneName);

  return domain;
}


// Helpers for creating operations.
template <typename TResources, typename TOperationID, typename TOffer>
inline typename TOffer::Operation RESERVE(
    const TResources& resources,
    const Option<TOperationID>& operationId = None())
{
  typename TOffer::Operation operation;
  operation.set_type(TOffer::Operation::RESERVE);
  operation.mutable_reserve()->mutable_resources()->CopyFrom(resources);

  if (operationId.isSome()) {
    *operation.mutable_id() = operationId.get();
  }

  return operation;
}


template <typename TResources, typename TOperationID, typename TOffer>
inline typename TOffer::Operation UNRESERVE(
    const TResources& resources,
    const Option<TOperationID>& operationId = None())
{
  typename TOffer::Operation operation;
  operation.set_type(TOffer::Operation::UNRESERVE);
  operation.mutable_unreserve()->mutable_resources()->CopyFrom(resources);

  if (operationId.isSome()) {
    *operation.mutable_id() = operationId.get();
  }

  return operation;
}


template <typename TResources, typename TOperationID, typename TOffer>
inline typename TOffer::Operation CREATE(
    const TResources& volumes,
    const Option<TOperationID>& operationId = None())
{
  typename TOffer::Operation operation;
  operation.set_type(TOffer::Operation::CREATE);
  operation.mutable_create()->mutable_volumes()->CopyFrom(volumes);

  if (operationId.isSome()) {
    *operation.mutable_id() = operationId.get();
  }

  return operation;
}


template <typename TResources, typename TOperationID, typename TOffer>
inline typename TOffer::Operation DESTROY(
    const TResources& volumes,
    const Option<TOperationID>& operationId = None())
{
  typename TOffer::Operation operation;
  operation.set_type(TOffer::Operation::DESTROY);
  operation.mutable_destroy()->mutable_volumes()->CopyFrom(volumes);

  if (operationId.isSome()) {
    *operation.mutable_id() = operationId.get();
  }

  return operation;
}


template <typename TResource, typename TOperationID, typename TOffer>
inline typename TOffer::Operation GROW_VOLUME(
    const TResource& volume,
    const TResource& addition,
    const Option<TOperationID>& operationId = None())
{
  typename TOffer::Operation operation;
  operation.set_type(TOffer::Operation::GROW_VOLUME);
  operation.mutable_grow_volume()->mutable_volume()->CopyFrom(volume);
  operation.mutable_grow_volume()->mutable_addition()->CopyFrom(addition);

  if (operationId.isSome()) {
    *operation.mutable_id() = operationId.get();
  }

  return operation;
}


template <
    typename TResource,
    typename TValueScalar,
    typename TOperationID,
    typename TOffer>
inline typename TOffer::Operation SHRINK_VOLUME(
    const TResource& volume,
    const TValueScalar& subtract,
    const Option<TOperationID>& operationId = None())
{
  typename TOffer::Operation operation;
  operation.set_type(TOffer::Operation::SHRINK_VOLUME);
  operation.mutable_shrink_volume()->mutable_volume()->CopyFrom(volume);
  operation.mutable_shrink_volume()->mutable_subtract()->CopyFrom(subtract);

  if (operationId.isSome()) {
    *operation.mutable_id() = operationId.get();
  }

  return operation;
}


template <typename TOffer, typename TTaskInfo>
inline typename TOffer::Operation LAUNCH(const std::vector<TTaskInfo>& tasks)
{
  typename TOffer::Operation operation;
  operation.set_type(TOffer::Operation::LAUNCH);

  foreach (const TTaskInfo& task, tasks) {
    operation.mutable_launch()->add_task_infos()->CopyFrom(task);
  }

  return operation;
}


template <typename TExecutorInfo, typename TTaskGroupInfo, typename TOffer>
inline typename TOffer::Operation LAUNCH_GROUP(
    const TExecutorInfo& executorInfo,
    const TTaskGroupInfo& taskGroup)
{
  typename TOffer::Operation operation;
  operation.set_type(TOffer::Operation::LAUNCH_GROUP);
  operation.mutable_launch_group()->mutable_executor()->CopyFrom(executorInfo);
  operation.mutable_launch_group()->mutable_task_group()->CopyFrom(taskGroup);
  return operation;
}


template <
    typename TResource,
    typename TTargetType,
    typename TOperationID,
    typename TOffer>
inline typename TOffer::Operation CREATE_DISK(
    const TResource& source,
    const TTargetType& targetType,
    const Option<std::string>& targetProfile = None(),
    const Option<TOperationID>& operationId = None())
{
  typename TOffer::Operation operation;
  operation.set_type(TOffer::Operation::CREATE_DISK);
  operation.mutable_create_disk()->mutable_source()->CopyFrom(source);
  operation.mutable_create_disk()->set_target_type(targetType);

  if (targetProfile.isSome()) {
    operation.mutable_create_disk()->set_target_profile(targetProfile.get());
  }

  if (operationId.isSome()) {
    operation.mutable_id()->CopyFrom(operationId.get());
  }

  return operation;
}


template <typename TResource, typename TOperationID, typename TOffer>
inline typename TOffer::Operation DESTROY_DISK(
    const TResource& source, const Option<TOperationID>& operationId = None())
{
  typename TOffer::Operation operation;
  operation.set_type(TOffer::Operation::DESTROY_DISK);
  operation.mutable_destroy_disk()->mutable_source()->CopyFrom(source);

  if (operationId.isSome()) {
    operation.mutable_id()->CopyFrom(operationId.get());
  }

  return operation;
}


template <typename TParameters, typename TParameter>
inline TParameters parameterize(const ACLs& acls)
{
  TParameters parameters;
  TParameter* parameter = parameters.add_parameter();
  parameter->set_key("acls");
  parameter->set_value(std::string(jsonify(JSON::Protobuf(acls))));

  return parameters;
}
} // namespace common {


// TODO(jmlvanre): Remove `inline` once we have adjusted all tests to
// distinguish between `internal` and `v1`.
inline namespace internal {
template <typename... Args>
inline ExecutorInfo createExecutorInfo(Args&&... args)
{
  return common::createExecutorInfo<
      ExecutorInfo,
      ExecutorID,
      Resources,
      CommandInfo,
      FrameworkID>(std::forward<Args>(args)...);
}


// We specify the argument to allow brace initialized construction.
inline CommandInfo createCommandInfo(
    const Option<std::string>& value = None(),
    const std::vector<std::string>& arguments = {})
{
  return common::createCommandInfo<CommandInfo>(value, arguments);
}


// Almost a direct snippet of code at the bottom of `Slave::launchExecutor`.
inline mesos::slave::ContainerConfig createContainerConfig(
    const Option<TaskInfo>& taskInfo,
    const ExecutorInfo& executorInfo,
    const std::string& sandboxDirectory,
    const Option<std::string>& user = None())
{
  mesos::slave::ContainerConfig containerConfig;
  containerConfig.mutable_executor_info()->CopyFrom(executorInfo);
  containerConfig.mutable_command_info()->CopyFrom(executorInfo.command());
  containerConfig.mutable_resources()->CopyFrom(executorInfo.resources());
  containerConfig.set_directory(sandboxDirectory);

  if (user.isSome()) {
    containerConfig.set_user(user.get());
  }

  if (taskInfo.isSome()) {
    containerConfig.mutable_task_info()->CopyFrom(taskInfo.get());

    if (taskInfo->has_container()) {
      containerConfig.mutable_container_info()->CopyFrom(taskInfo->container());
    }
  } else {
    if (executorInfo.has_container()) {
      containerConfig.mutable_container_info()
        ->CopyFrom(executorInfo.container());
    }
  }

  return containerConfig;
}


// Almost a direct snippet of code in `Slave::Http::_launchNestedContainer`.
inline mesos::slave::ContainerConfig createContainerConfig(
    const CommandInfo& commandInfo,
    const Option<ContainerInfo>& containerInfo = None(),
    const Option<mesos::slave::ContainerClass>& containerClass = None(),
    const Option<std::string>& user = None())
{
  mesos::slave::ContainerConfig containerConfig;
  containerConfig.mutable_command_info()->CopyFrom(commandInfo);

  if (user.isSome()) {
    containerConfig.set_user(user.get());
  }

  if (containerInfo.isSome()) {
    containerConfig.mutable_container_info()->CopyFrom(containerInfo.get());
  }

  if (containerClass.isSome()) {
    containerConfig.set_container_class(containerClass.get());
  }

  return containerConfig;
}


// Helper for creating standalone container configs.
inline mesos::slave::ContainerConfig createContainerConfig(
    const CommandInfo& commandInfo,
    const std::string& resources,
    const std::string& sandboxDirectory,
    const Option<ContainerInfo>& containerInfo = None(),
    const Option<std::string>& user = None())
{
  mesos::slave::ContainerConfig containerConfig;
  containerConfig.mutable_command_info()->CopyFrom(commandInfo);
  containerConfig.mutable_resources()->CopyFrom(
      Resources::parse(resources).get());

  containerConfig.set_directory(sandboxDirectory);

  if (user.isSome()) {
    containerConfig.set_user(user.get());
  }

  if (containerInfo.isSome()) {
    containerConfig.mutable_container_info()->CopyFrom(containerInfo.get());
  }

  return containerConfig;
}


template <typename... Args>
inline Image createDockerImage(Args&&... args)
{
  return common::createDockerImage<Image>(std::forward<Args>(args)...);
}


template <typename... Args>
inline Volume createVolumeSandboxPath(Args&&... args)
{
  return common::createVolumeSandboxPath<Volume>(std::forward<Args>(args)...);
}


template <typename... Args>
inline Volume createVolumeHostPath(Args&&... args)
{
  return common::createVolumeHostPath<Volume, MountPropagation>(
      std::forward<Args>(args)...);
}


template <typename... Args>
inline Volume createVolumeFromDockerImage(Args&&... args)
{
  return common::createVolumeFromDockerImage<Volume, Image>(
      std::forward<Args>(args)...);
}


template <typename... Args>
inline Volume createVolumeCsi(Args&&... args)
{
  return common::createVolumeCsi<Volume>(
      std::forward<Args>(args)...);
}


template <typename... Args>
inline NetworkInfo createNetworkInfo(Args&&... args)
{
  return common::createNetworkInfo<NetworkInfo>(std::forward<Args>(args)...);
}


// We specify the argument to allow brace initialized construction.
inline ContainerInfo createContainerInfo(
    const Option<std::string>& imageName = None(),
    const std::vector<Volume>& volumes = {})
{
  return common::createContainerInfo<ContainerInfo, Volume, Image>(
      imageName,
      volumes);
}


template <typename... Args>
inline TaskInfo createTask(Args&&... args)
{
  return common::createTask<
      TaskInfo,
      ExecutorID,
      SlaveID,
      Resources,
      ExecutorInfo,
      CommandInfo,
      Offer,
      Value::Scalar>(std::forward<Args>(args)...);
}


// We specify the argument to allow brace initialized construction.
inline TaskGroupInfo createTaskGroupInfo(const std::vector<TaskInfo>& tasks)
{
  return common::createTaskGroupInfo<TaskGroupInfo, TaskInfo>(tasks);
}


inline Resource::ReservationInfo createStaticReservationInfo(
    const std::string& role)
{
  return common::createStaticReservationInfo<Resource>(role);
}


inline Resource::ReservationInfo createDynamicReservationInfo(
    const std::string& role,
    const Option<std::string>& principal = None(),
    const Option<Labels>& labels = None())
{
  return common::createDynamicReservationInfo<Resource, Labels>(
      role, principal, labels);
}


template <typename... Args>
inline Resource createReservedResource(Args&&... args)
{
  return common::createReservedResource<Resource, Resources>(
      std::forward<Args>(args)...);
}


template <typename... Args>
inline Resource::DiskInfo createDiskInfo(Args&&... args)
{
  return common::createDiskInfo<Resource, Volume>(std::forward<Args>(args)...);
}


template <typename... Args>
inline Resource::DiskInfo::Source createDiskSourcePath(Args&&... args)
{
  return common::createDiskSourcePath<Resource>(std::forward<Args>(args)...);
}


template <typename... Args>
inline Resource::DiskInfo::Source createDiskSourceMount(Args&&... args)
{
  return common::createDiskSourceMount<Resource>(std::forward<Args>(args)...);
}


template <typename... Args>
inline Resource::DiskInfo::Source createDiskSourceBlock(Args&&... args)
{
  return common::createDiskSourceBlock<Resource>(std::forward<Args>(args)...);
}


template <typename... Args>
inline Resource::DiskInfo::Source createDiskSourceRaw(Args&&... args)
{
  return common::createDiskSourceRaw<Resource>(std::forward<Args>(args)...);
}


template <typename... Args>
inline Resource createDiskResource(Args&&... args)
{
  return common::createDiskResource<Resource, Resources, Volume>(
      std::forward<Args>(args)...);
}


template <typename... Args>
inline Resource createPersistentVolume(Args&&... args)
{
  return common::createPersistentVolume<Resource, Resources, Volume>(
      std::forward<Args>(args)...);
}


template <typename... Args>
inline process::http::Headers createBasicAuthHeaders(Args&&... args)
{
  return common::createBasicAuthHeaders<Credential>(
      std::forward<Args>(args)...);
}


template <typename... Args>
inline google::protobuf::RepeatedPtrField<WeightInfo> createWeightInfos(
    Args&&... args)
{
  return common::createWeightInfos<WeightInfo>(std::forward<Args>(args)...);
}


template <typename... Args>
inline hashmap<std::string, double> convertToHashmap(Args&&... args)
{
  return common::convertToHashmap<WeightInfo>(std::forward<Args>(args)...);
}


template <typename... Args>
inline DomainInfo createDomainInfo(Args&&... args)
{
  return common::createDomainInfo<DomainInfo>(std::forward<Args>(args)...);
}


template <typename... Args>
inline Offer::Operation RESERVE(Args&&... args)
{
  return common::RESERVE<Resources, OperationID, Offer>(
      std::forward<Args>(args)...);
}


template <typename... Args>
inline Offer::Operation UNRESERVE(Args&&... args)
{
  return common::UNRESERVE<Resources, OperationID, Offer>(
      std::forward<Args>(args)...);
}


template <typename... Args>
inline Offer::Operation CREATE(Args&&... args)
{
  return common::CREATE<Resources, OperationID, Offer>(
      std::forward<Args>(args)...);
}


template <typename... Args>
inline Offer::Operation DESTROY(Args&&... args)
{
  return common::DESTROY<Resources, OperationID, Offer>(
      std::forward<Args>(args)...);
}


template <typename... Args>
inline Offer::Operation GROW_VOLUME(Args&&... args)
{
  return common::GROW_VOLUME<Resource, OperationID, Offer>(
      std::forward<Args>(args)...);
}


template <typename... Args>
inline Offer::Operation SHRINK_VOLUME(Args&&... args)
{
  return common::SHRINK_VOLUME<Resource, Value::Scalar, OperationID, Offer>(
      std::forward<Args>(args)...);
}


// We specify the argument to allow brace initialized construction.
inline Offer::Operation LAUNCH(const std::vector<TaskInfo>& tasks)
{
  return common::LAUNCH<Offer, TaskInfo>(tasks);
}


template <typename... Args>
inline Offer::Operation LAUNCH_GROUP(Args&&... args)
{
  return common::LAUNCH_GROUP<ExecutorInfo, TaskGroupInfo, Offer>(
      std::forward<Args>(args)...);
}


template <typename... Args>
inline Offer::Operation CREATE_DISK(Args&&... args)
{
  return common::
    CREATE_DISK<Resource, Resource::DiskInfo::Source::Type, OperationID, Offer>(
        std::forward<Args>(args)...);
}


template <typename... Args>
inline Offer::Operation DESTROY_DISK(Args&&... args)
{
  return common::DESTROY_DISK<Resource, OperationID, Offer>(
      std::forward<Args>(args)...);
}


template <typename... Args>
inline Parameters parameterize(Args&&... args)
{
  return common::parameterize<Parameters, Parameter>(
      std::forward<Args>(args)...);
}
} // namespace internal {


namespace v1 {
template <typename... Args>
inline mesos::v1::ExecutorInfo createExecutorInfo(Args&&... args)
{
  return common::createExecutorInfo<
      mesos::v1::ExecutorInfo,
      mesos::v1::ExecutorID,
      mesos::v1::Resources,
      mesos::v1::CommandInfo,
      mesos::v1::FrameworkID>(std::forward<Args>(args)...);
}


// We specify the argument to allow brace initialized construction.
inline mesos::v1::CommandInfo createCommandInfo(
    const Option<std::string>& value = None(),
    const std::vector<std::string>& arguments = {})
{
  return common::createCommandInfo<mesos::v1::CommandInfo>(value, arguments);
}


template <typename... Args>
inline mesos::v1::Image createDockerImage(Args&&... args)
{
  return common::createDockerImage<mesos::v1::Image>(
      std::forward<Args>(args)...);
}


template <typename... Args>
inline mesos::v1::Volume createVolumeSandboxPath(Args&&... args)
{
  return common::createVolumeSandboxPath<mesos::v1::Volume>(
      std::forward<Args>(args)...);
}


template <typename... Args>
inline mesos::v1::Volume createVolumeHostPath(Args&&... args)
{
  return common::createVolumeHostPath<
      mesos::v1::Volume,
      mesos::v1::MountPropagation>(std::forward<Args>(args)...);
}


template <typename... Args>
inline mesos::v1::Volume createVolumeFromDockerImage(Args&&... args)
{
  return common::createVolumeFromDockerImage<
      mesos::v1::Volume, mesos::v1::Image>(std::forward<Args>(args)...);
}


template <typename... Args>
inline mesos::v1::Volume createVolumeCsi(Args&&... args)
{
  return common::createVolumeCsi<mesos::v1::Volume>(
      std::forward<Args>(args)...);
}


template <typename... Args>
inline mesos::v1::NetworkInfo createNetworkInfo(Args&&... args)
{
  return common::createNetworkInfo<mesos::v1::NetworkInfo>(
      std::forward<Args>(args)...);
}


// We specify the argument to allow brace initialized construction.
inline mesos::v1::ContainerInfo createContainerInfo(
    const Option<std::string>& imageName = None(),
    const std::vector<mesos::v1::Volume>& volumes = {})
{
  return common::createContainerInfo<
      mesos::v1::ContainerInfo, mesos::v1::Volume, mesos::v1::Image>(
          imageName, volumes);
}


template <typename... Args>
inline mesos::v1::TaskInfo createTask(Args&&... args)
{
  return common::createTask<
      mesos::v1::TaskInfo,
      mesos::v1::ExecutorID,
      mesos::v1::AgentID,
      mesos::v1::Resources,
      mesos::v1::ExecutorInfo,
      mesos::v1::CommandInfo,
      mesos::v1::Offer,
      mesos::v1::Value::Scalar>(std::forward<Args>(args)...);
}


// We specify the argument to allow brace initialized construction.
inline mesos::v1::TaskGroupInfo createTaskGroupInfo(
    const std::vector<mesos::v1::TaskInfo>& tasks)
{
  return common::createTaskGroupInfo<
      mesos::v1::TaskGroupInfo,
      mesos::v1::TaskInfo>(tasks);
}


inline mesos::v1::Resource::ReservationInfo createStaticReservationInfo(
    const std::string& role)
{
  return common::createStaticReservationInfo<mesos::v1::Resource>(role);
}


inline mesos::v1::Resource::ReservationInfo createDynamicReservationInfo(
    const std::string& role,
    const Option<std::string>& principal = None(),
    const Option<mesos::v1::Labels>& labels = None())
{
  return common::createDynamicReservationInfo<
             mesos::v1::Resource, mesos::v1::Labels>(role, principal, labels);
}


template <typename... Args>
inline mesos::v1::Resource createReservedResource(Args&&... args)
{
  return common::createReservedResource<
      mesos::v1::Resource, mesos::v1::Resources>(std::forward<Args>(args)...);
}


template <typename... Args>
inline mesos::v1::Resource::DiskInfo createDiskInfo(Args&&... args)
{
  return common::createDiskInfo<mesos::v1::Resource, mesos::v1::Volume>(
      std::forward<Args>(args)...);
}


template <typename... Args>
inline mesos::v1::Resource::DiskInfo::Source createDiskSourcePath(
    Args&&... args)
{
  return common::createDiskSourcePath<mesos::v1::Resource>(
      std::forward<Args>(args)...);
}


template <typename... Args>
inline mesos::v1::Resource::DiskInfo::Source createDiskSourceMount(
    Args&&... args)
{
  return common::createDiskSourceMount<mesos::v1::Resource>(
      std::forward<Args>(args)...);
}


template <typename... Args>
inline mesos::v1::Resource::DiskInfo::Source createDiskSourceBlock(
    Args&&... args)
{
  return common::createDiskSourceBlock<mesos::v1::Resource>(
      std::forward<Args>(args)...);
}


template <typename... Args>
inline mesos::v1::Resource::DiskInfo::Source createDiskSourceRaw(
    Args&&... args)
{
  return common::createDiskSourceRaw<mesos::v1::Resource>(
      std::forward<Args>(args)...);
}


template <typename... Args>
inline mesos::v1::Resource createDiskResource(Args&&... args)
{
  return common::createDiskResource<
      mesos::v1::Resource,
      mesos::v1::Resources,
      mesos::v1::Volume>(std::forward<Args>(args)...);
}


template <typename... Args>
inline mesos::v1::Resource createPersistentVolume(Args&&... args)
{
  return common::createPersistentVolume<
      mesos::v1::Resource,
      mesos::v1::Resources,
      mesos::v1::Volume>(std::forward<Args>(args)...);
}


template <typename... Args>
inline process::http::Headers createBasicAuthHeaders(Args&&... args)
{
  return common::createBasicAuthHeaders<mesos::v1::Credential>(
      std::forward<Args>(args)...);
}


template <typename... Args>
inline google::protobuf::RepeatedPtrField<
    mesos::v1::WeightInfo> createWeightInfos(Args&&... args)
{
  return common::createWeightInfos<mesos::v1::WeightInfo>(
      std::forward<Args>(args)...);
}


template <typename... Args>
inline hashmap<std::string, double> convertToHashmap(Args&&... args)
{
  return common::convertToHashmap<mesos::v1::WeightInfo>(
      std::forward<Args>(args)...);
}


template <typename... Args>
inline mesos::v1::Offer::Operation RESERVE(Args&&... args)
{
  return common::RESERVE<
      mesos::v1::Resources,
      mesos::v1::OperationID,
      mesos::v1::Offer>(std::forward<Args>(args)...);
}


template <typename... Args>
inline mesos::v1::Offer::Operation UNRESERVE(Args&&... args)
{
  return common::UNRESERVE<
      mesos::v1::Resources,
      mesos::v1::OperationID,
      mesos::v1::Offer>(std::forward<Args>(args)...);
}


template <typename... Args>
inline mesos::v1::Offer::Operation CREATE(Args&&... args)
{
  return common::CREATE<
      mesos::v1::Resources,
      mesos::v1::OperationID,
      mesos::v1::Offer>(std::forward<Args>(args)...);
}


template <typename... Args>
inline mesos::v1::Offer::Operation DESTROY(Args&&... args)
{
  return common::DESTROY<
      mesos::v1::Resources,
      mesos::v1::OperationID,
      mesos::v1::Offer>(std::forward<Args>(args)...);
}


template <typename... Args>
inline mesos::v1::Offer::Operation GROW_VOLUME(Args&&... args)
{
  return common::GROW_VOLUME<
      mesos::v1::Resource,
      mesos::v1::OperationID,
      mesos::v1::Offer>(std::forward<Args>(args)...);
}


template <typename... Args>
inline mesos::v1::Offer::Operation SHRINK_VOLUME(Args&&... args)
{
  return common::SHRINK_VOLUME<
      mesos::v1::Resource,
      mesos::v1::Value::Scalar,
      mesos::v1::OperationID,
      mesos::v1::Offer>(std::forward<Args>(args)...);
}


// We specify the argument to allow brace initialized construction.
inline mesos::v1::Offer::Operation LAUNCH(
    const std::vector<mesos::v1::TaskInfo>& tasks)
{
  return common::LAUNCH<mesos::v1::Offer, mesos::v1::TaskInfo>(tasks);
}


template <typename... Args>
inline mesos::v1::Offer::Operation LAUNCH_GROUP(Args&&... args)
{
  return common::LAUNCH_GROUP<
      mesos::v1::ExecutorInfo,
      mesos::v1::TaskGroupInfo,
      mesos::v1::Offer>(std::forward<Args>(args)...);
}


template <typename... Args>
inline mesos::v1::Offer::Operation CREATE_DISK(Args&&... args)
{
  return common::CREATE_DISK<
      mesos::v1::Resource,
      mesos::v1::Resource::DiskInfo::Source::Type,
      mesos::v1::OperationID,
      mesos::v1::Offer>(std::forward<Args>(args)...);
}


template <typename... Args>
inline mesos::v1::Offer::Operation DESTROY_DISK(Args&&... args)
{
  return common::DESTROY_DISK<
      mesos::v1::Resource,
      mesos::v1::OperationID,
      mesos::v1::Offer>(std::forward<Args>(args)...);
}


template <typename... Args>
inline mesos::v1::Parameters parameterize(Args&&... args)
{
  return common::parameterize<mesos::v1::Parameters, mesos::v1::Parameter>(
      std::forward<Args>(args)...);
}


inline mesos::v1::scheduler::Call createCallAccept(
    const mesos::v1::FrameworkID& frameworkId,
    const mesos::v1::Offer& offer,
    const std::vector<mesos::v1::Offer::Operation>& operations,
    const Option<mesos::v1::Filters>& filters = None())
{
  mesos::v1::scheduler::Call call;
  call.set_type(mesos::v1::scheduler::Call::ACCEPT);
  call.mutable_framework_id()->CopyFrom(frameworkId);

  mesos::v1::scheduler::Call::Accept* accept = call.mutable_accept();
  accept->add_offer_ids()->CopyFrom(offer.id());

  foreach (const mesos::v1::Offer::Operation& operation, operations) {
    accept->add_operations()->CopyFrom(operation);
  }

  if (filters.isSome()) {
    accept->mutable_filters()->CopyFrom(filters.get());
  }

  return call;
}


inline mesos::v1::scheduler::Call createCallAcknowledge(
    const mesos::v1::FrameworkID& frameworkId,
    const mesos::v1::AgentID& agentId,
    const mesos::v1::scheduler::Event::Update& update)
{
  mesos::v1::scheduler::Call call;
  call.set_type(mesos::v1::scheduler::Call::ACKNOWLEDGE);
  call.mutable_framework_id()->CopyFrom(frameworkId);

  mesos::v1::scheduler::Call::Acknowledge* acknowledge =
    call.mutable_acknowledge();

  acknowledge->mutable_task_id()->CopyFrom(
      update.status().task_id());

  acknowledge->mutable_agent_id()->CopyFrom(agentId);
  acknowledge->set_uuid(update.status().uuid());

  return call;
}


inline mesos::v1::scheduler::Call createCallAcknowledgeOperationStatus(
    const mesos::v1::FrameworkID& frameworkId,
    const mesos::v1::AgentID& agentId,
    const Option<mesos::v1::ResourceProviderID>& resourceProviderId,
    const mesos::v1::scheduler::Event::UpdateOperationStatus& update)
{
  mesos::v1::scheduler::Call call;
  call.set_type(mesos::v1::scheduler::Call::ACKNOWLEDGE_OPERATION_STATUS);
  call.mutable_framework_id()->CopyFrom(frameworkId);

  mesos::v1::scheduler::Call::AcknowledgeOperationStatus* acknowledge =
    call.mutable_acknowledge_operation_status();

  acknowledge->mutable_agent_id()->CopyFrom(agentId);
  if (resourceProviderId.isSome()) {
    acknowledge->mutable_resource_provider_id()->CopyFrom(
        resourceProviderId.get());
  }
  acknowledge->set_uuid(update.status().uuid().value());
  acknowledge->mutable_operation_id()->CopyFrom(update.status().operation_id());

  return call;
}


inline mesos::v1::scheduler::Call createCallKill(
    const mesos::v1::FrameworkID& frameworkId,
    const mesos::v1::TaskID& taskId,
    const Option<mesos::v1::AgentID>& agentId = None(),
    const Option<mesos::v1::KillPolicy>& killPolicy = None())
{
  mesos::v1::scheduler::Call call;
  call.set_type(mesos::v1::scheduler::Call::KILL);
  call.mutable_framework_id()->CopyFrom(frameworkId);

  mesos::v1::scheduler::Call::Kill* kill = call.mutable_kill();
  kill->mutable_task_id()->CopyFrom(taskId);

  if (agentId.isSome()) {
    kill->mutable_agent_id()->CopyFrom(agentId.get());
  }

  if (killPolicy.isSome()) {
    kill->mutable_kill_policy()->CopyFrom(killPolicy.get());
  }

  return call;
}


inline mesos::v1::scheduler::Call createCallReconcileOperations(
    const mesos::v1::FrameworkID& frameworkId,
    const std::vector<
        mesos::v1::scheduler::Call::ReconcileOperations::Operation>&
      operations = {})
{
  mesos::v1::scheduler::Call call;
  call.set_type(mesos::v1::scheduler::Call::RECONCILE_OPERATIONS);
  call.mutable_framework_id()->CopyFrom(frameworkId);

  mesos::v1::scheduler::Call::ReconcileOperations* reconcile =
    call.mutable_reconcile_operations();

  foreach (
      const mesos::v1::scheduler::Call::ReconcileOperations::Operation&
        operation,
      operations) {
    reconcile->add_operations()->CopyFrom(operation);
  }

  return call;
}


inline mesos::v1::scheduler::Call createCallSubscribe(
  const mesos::v1::FrameworkInfo& frameworkInfo,
  const Option<mesos::v1::FrameworkID>& frameworkId = None())
{
  mesos::v1::scheduler::Call call;
  call.set_type(mesos::v1::scheduler::Call::SUBSCRIBE);

  call.mutable_subscribe()->mutable_framework_info()->CopyFrom(frameworkInfo);

  if (frameworkId.isSome()) {
    call.mutable_framework_id()->CopyFrom(frameworkId.get());
  }

  return call;
}

} // namespace v1 {


inline mesos::Environment createEnvironment(
    const hashmap<std::string, std::string>& map)
{
  mesos::Environment environment;
  foreachpair (const std::string& key, const std::string& value, map) {
    mesos::Environment::Variable* variable = environment.add_variables();
    variable->set_name(key);
    variable->set_value(value);
  }
  return environment;
}


// Macros to get/create (default) ExecutorInfos and FrameworkInfos.
#define DEFAULT_EXECUTOR_INFO createExecutorInfo("default", "exit 1")


#define DEFAULT_CREDENTIAL DefaultCredential::create()
#define DEFAULT_CREDENTIAL_2 DefaultCredential2::create()


#define DEFAULT_FRAMEWORK_INFO DefaultFrameworkInfo::create()


#define DEFAULT_EXECUTOR_ID DEFAULT_EXECUTOR_INFO.executor_id()


// Definition of a mock Scheduler to be used in tests with gmock.
class MockScheduler : public Scheduler
{
public:
  MockScheduler();
  ~MockScheduler() override;

  MOCK_METHOD3(registered, void(SchedulerDriver*,
                                const FrameworkID&,
                                const MasterInfo&));
  MOCK_METHOD2(reregistered, void(SchedulerDriver*, const MasterInfo&));
  MOCK_METHOD1(disconnected, void(SchedulerDriver*));
  MOCK_METHOD2(resourceOffers, void(SchedulerDriver*,
                                    const std::vector<Offer>&));
  MOCK_METHOD2(offerRescinded, void(SchedulerDriver*, const OfferID&));
  MOCK_METHOD2(statusUpdate, void(SchedulerDriver*, const TaskStatus&));
  MOCK_METHOD4(frameworkMessage, void(SchedulerDriver*,
                                      const ExecutorID&,
                                      const SlaveID&,
                                      const std::string&));
  MOCK_METHOD2(slaveLost, void(SchedulerDriver*, const SlaveID&));
  MOCK_METHOD4(executorLost, void(SchedulerDriver*,
                                  const ExecutorID&,
                                  const SlaveID&,
                                  int));
  MOCK_METHOD2(error, void(SchedulerDriver*, const std::string&));
};

// For use with a MockScheduler, for example:
// EXPECT_CALL(sched, resourceOffers(_, _))
//   .WillOnce(LaunchTasks(EXECUTOR, TASKS, CPUS, MEM, ROLE));
// Launches up to TASKS no-op tasks, if possible,
// each with CPUS cpus and MEM memory and EXECUTOR executor.
ACTION_P5(LaunchTasks, executor, tasks, cpus, mem, role)
{
  SchedulerDriver* driver = arg0;
  std::vector<Offer> offers = arg1;
  int numTasks = tasks;

  int launched = 0;
  for (size_t i = 0; i < offers.size(); i++) {
    const Offer& offer = offers[i];

    Resources taskResources = Resources::parse(
        "cpus:" + stringify(cpus) + ";mem:" + stringify(mem)).get();

    if (offer.resources_size() > 0 &&
        offer.resources(0).has_allocation_info()) {
      taskResources.allocate(role);
    }

    std::vector<TaskInfo> tasks;
    Resources remaining = offer.resources();

    while (remaining.toUnreserved().contains(taskResources) &&
           launched < numTasks) {
      TaskInfo task;
      task.set_name("TestTask");
      task.mutable_task_id()->set_value(id::UUID::random().toString());
      task.mutable_slave_id()->MergeFrom(offer.slave_id());
      task.mutable_executor()->MergeFrom(executor);

      Option<Resources> resources = remaining.find(
          role == std::string("*")
            ? taskResources
            : taskResources.pushReservation(createStaticReservationInfo(role)));

      CHECK_SOME(resources);

      task.mutable_resources()->MergeFrom(resources.get());
      remaining -= resources.get();

      tasks.push_back(task);
      launched++;
    }

    driver->launchTasks(offer.id(), tasks);
  }
}


// Like LaunchTasks, but decline the entire offer and
// don't launch any tasks.
ACTION(DeclineOffers)
{
  SchedulerDriver* driver = arg0;
  std::vector<Offer> offers = arg1;

  for (size_t i = 0; i < offers.size(); i++) {
    driver->declineOffer(offers[i].id());
  }
}


// Like DeclineOffers, but takes a custom filters object.
ACTION_P(DeclineOffers, filters)
{
  SchedulerDriver* driver = arg0;
  std::vector<Offer> offers = arg1;

  for (size_t i = 0; i < offers.size(); i++) {
    driver->declineOffer(offers[i].id(), filters);
  }
}


// For use with a MockScheduler, for example:
// process::Queue<Offer> offers;
// EXPECT_CALL(sched, resourceOffers(_, _))
//   .WillRepeatedly(EnqueueOffers(&offers));
// Enqueues all received offers into the provided queue.
ACTION_P(EnqueueOffers, queue)
{
  std::vector<Offer> offers = arg1;
  foreach (const Offer& offer, offers) {
    queue->put(offer);
  }
}


// Definition of a mock Executor to be used in tests with gmock.
class MockExecutor : public Executor
{
public:
  MockExecutor(const ExecutorID& _id);
  ~MockExecutor() override;

  MOCK_METHOD4(registered, void(ExecutorDriver*,
                                const ExecutorInfo&,
                                const FrameworkInfo&,
                                const SlaveInfo&));
  MOCK_METHOD2(reregistered, void(ExecutorDriver*, const SlaveInfo&));
  MOCK_METHOD1(disconnected, void(ExecutorDriver*));
  MOCK_METHOD2(launchTask, void(ExecutorDriver*, const TaskInfo&));
  MOCK_METHOD2(killTask, void(ExecutorDriver*, const TaskID&));
  MOCK_METHOD2(frameworkMessage, void(ExecutorDriver*, const std::string&));
  MOCK_METHOD1(shutdown, void(ExecutorDriver*));
  MOCK_METHOD2(error, void(ExecutorDriver*, const std::string&));

  const ExecutorID id;
};


class TestingMesosSchedulerDriver : public MesosSchedulerDriver
{
public:
  TestingMesosSchedulerDriver(
      Scheduler* scheduler,
      mesos::master::detector::MasterDetector* _detector)
    : MesosSchedulerDriver(
          scheduler,
          internal::DEFAULT_FRAMEWORK_INFO,
          "",
          true,
          internal::DEFAULT_CREDENTIAL)
  {
    // No-op destructor as _detector lives on the stack.
    detector =
      std::shared_ptr<mesos::master::detector::MasterDetector>(
          _detector, [](mesos::master::detector::MasterDetector*) {});
  }

  TestingMesosSchedulerDriver(
      Scheduler* scheduler,
      mesos::master::detector::MasterDetector* _detector,
      const FrameworkInfo& framework,
      bool implicitAcknowledgements = true)
    : MesosSchedulerDriver(
          scheduler,
          framework,
          "",
          implicitAcknowledgements,
          internal::DEFAULT_CREDENTIAL)
  {
    // No-op destructor as _detector lives on the stack.
    detector =
      std::shared_ptr<mesos::master::detector::MasterDetector>(
          _detector, [](mesos::master::detector::MasterDetector*) {});
  }

  TestingMesosSchedulerDriver(
      Scheduler* scheduler,
      mesos::master::detector::MasterDetector* _detector,
      const FrameworkInfo& framework,
      bool implicitAcknowledgements,
      const Credential& credential)
    : MesosSchedulerDriver(
          scheduler,
          framework,
          "",
          implicitAcknowledgements,
          credential)
  {
    // No-op destructor as _detector lives on the stack.
    detector =
      std::shared_ptr<mesos::master::detector::MasterDetector>(
          _detector, [](mesos::master::detector::MasterDetector*) {});
  }
};


namespace scheduler {

// A generic mock HTTP scheduler to be used in tests with gmock.
template <typename Mesos, typename Event>
class MockHTTPScheduler
{
public:
  MOCK_METHOD1_T(connected, void(Mesos*));
  MOCK_METHOD1_T(disconnected, void(Mesos*));
  MOCK_METHOD1_T(heartbeat, void(Mesos*));
  MOCK_METHOD2_T(subscribed, void(Mesos*, const typename Event::Subscribed&));
  MOCK_METHOD2_T(offers, void(Mesos*, const typename Event::Offers&));
  MOCK_METHOD2_T(
      inverseOffers,
      void(Mesos*, const typename Event::InverseOffers&));
  MOCK_METHOD2_T(rescind, void(Mesos*, const typename Event::Rescind&));
  MOCK_METHOD2_T(
      rescindInverseOffers,
      void(Mesos*, const typename Event::RescindInverseOffer&));
  MOCK_METHOD2_T(update, void(Mesos*, const typename Event::Update&));
  MOCK_METHOD2_T(
      updateOperationStatus,
      void(Mesos*, const typename Event::UpdateOperationStatus&));
  MOCK_METHOD2_T(message, void(Mesos*, const typename Event::Message&));
  MOCK_METHOD2_T(failure, void(Mesos*, const typename Event::Failure&));
  MOCK_METHOD2_T(error, void(Mesos*, const typename Event::Error&));

  void events(Mesos* mesos, std::queue<Event> events)
  {
    while (!events.empty()) {
      Event event = std::move(events.front());
      events.pop();

      switch (event.type()) {
        case Event::SUBSCRIBED:
          subscribed(mesos, event.subscribed());
          break;
        case Event::OFFERS:
          offers(mesos, event.offers());
          break;
        case Event::INVERSE_OFFERS:
          inverseOffers(mesos, event.inverse_offers());
          break;
        case Event::RESCIND:
          rescind(mesos, event.rescind());
          break;
        case Event::RESCIND_INVERSE_OFFER:
          rescindInverseOffers(mesos, event.rescind_inverse_offer());
          break;
        case Event::UPDATE:
          update(mesos, event.update());
          break;
        case Event::UPDATE_OPERATION_STATUS:
          updateOperationStatus(mesos, event.update_operation_status());
          break;
        case Event::MESSAGE:
          message(mesos, event.message());
          break;
        case Event::FAILURE:
          failure(mesos, event.failure());
          break;
        case Event::ERROR:
          error(mesos, event.error());
          break;
        case Event::HEARTBEAT:
          heartbeat(mesos);
          break;
        case Event::UNKNOWN:
          LOG(FATAL) << "Received unexpected UNKNOWN event";
          break;
      }
    }
  }
};


// A generic testing interface for the scheduler library that can be used to
// test the library across various versions.
template <typename Mesos, typename Event>
class TestMesos : public Mesos
{
public:
  TestMesos(
      const std::string& master,
      ContentType contentType,
      const std::shared_ptr<MockHTTPScheduler<Mesos, Event>>& scheduler,
      const Option<std::shared_ptr<mesos::master::detector::MasterDetector>>&
          detector = None(),
      const mesos::v1::Credential& credential = v1::DEFAULT_CREDENTIAL)
    : Mesos(
          master,
          contentType,
          lambda::bind(&MockHTTPScheduler<Mesos, Event>::connected,
                       scheduler,
                       this),
          lambda::bind(&MockHTTPScheduler<Mesos, Event>::disconnected,
                       scheduler,
                       this),
          lambda::bind(&MockHTTPScheduler<Mesos, Event>::events,
                       scheduler,
                       this,
                       lambda::_1),
          credential,
          detector) {}

  ~TestMesos() override
  {
    // Since the destructor for `TestMesos` is invoked first, the library can
    // make more callbacks to the `scheduler` object before the `Mesos` (base
    // class) destructor is invoked. To prevent this, we invoke `stop()` here
    // to explicitly stop the library.
    this->stop();

    bool paused = process::Clock::paused();

    // Need to settle the Clock to ensure that all the pending async callbacks
    // with references to `this` and `scheduler` queued on libprocess are
    // executed before the object is destructed.
    process::Clock::pause();
    process::Clock::settle();

    // Return the Clock to its original state.
    if (!paused) {
      process::Clock::resume();
    }
  }
};

} // namespace scheduler {


namespace v1 {
namespace scheduler {

using APIResult = mesos::v1::scheduler::APIResult;
using Call = mesos::v1::scheduler::Call;
using Event = mesos::v1::scheduler::Event;
using Mesos = mesos::v1::scheduler::Mesos;
using Response = mesos::v1::scheduler::Response;


using TestMesos = tests::scheduler::TestMesos<
    mesos::v1::scheduler::Mesos,
    mesos::v1::scheduler::Event>;


// This matcher is used to match an offer event that contains a vector of offers
// having any resource that passes the filter.
MATCHER_P(OffersHaveAnyResource, filter, "")
{
  foreach (const Offer& offer, arg.offers()) {
    foreach (const Resource& resource, offer.resources()) {
      if (filter(resource)) {
        return true;
      }
    }
  }

  return false;
}


// This matcher is used to match the operation ID of an
// `Event.update_operation_status.status` message.
MATCHER_P(OperationStatusUpdateOperationIdEq, operationId, "")
{
  return arg.status().has_operation_id() &&
    arg.status().operation_id() == operationId;
}


// Like LaunchTasks, but decline the entire offer and don't launch any tasks.
ACTION(DeclineOffers)
{
  Call call;
  call.set_type(Call::DECLINE);

  Call::Decline* decline = call.mutable_decline();

  foreach (const Offer& offer, arg1.offers()) {
    decline->add_offer_ids()->CopyFrom(offer.id());

    if (!call.has_framework_id()) {
      call.mutable_framework_id()->CopyFrom(offer.framework_id());
    }
  }

  arg0->send(call);
}


ACTION_P(SendSubscribe, frameworkInfo)
{
  Call call;
  call.set_type(Call::SUBSCRIBE);
  call.mutable_subscribe()->mutable_framework_info()->CopyFrom(frameworkInfo);

  arg0->send(call);
}


ACTION_P2(SendSubscribe, frameworkInfo, frameworkId)
{
  Call call;
  call.set_type(Call::SUBSCRIBE);
  call.mutable_framework_id()->CopyFrom(frameworkId);
  call.mutable_subscribe()->mutable_framework_info()->CopyFrom(frameworkInfo);
  call.mutable_subscribe()->mutable_framework_info()->mutable_id()->CopyFrom(
      frameworkId);

  arg0->send(call);
}


ACTION_P2(SendAcknowledge, frameworkId, agentId)
{
  Call call;
  call.set_type(Call::ACKNOWLEDGE);
  call.mutable_framework_id()->CopyFrom(frameworkId);

  Call::Acknowledge* acknowledge = call.mutable_acknowledge();
  acknowledge->mutable_task_id()->CopyFrom(arg1.status().task_id());
  acknowledge->mutable_agent_id()->CopyFrom(agentId);
  acknowledge->set_uuid(arg1.status().uuid());

  arg0->send(call);
}


ACTION_P2(
    SendAcknowledgeOperationStatus, frameworkId, agentId)
{
  Call call;
  call.set_type(Call::ACKNOWLEDGE_OPERATION_STATUS);
  call.mutable_framework_id()->CopyFrom(frameworkId);

  Call::AcknowledgeOperationStatus* acknowledge =
    call.mutable_acknowledge_operation_status();

  acknowledge->mutable_agent_id()->CopyFrom(agentId);
  acknowledge->set_uuid(arg1.status().uuid().value());
  acknowledge->mutable_operation_id()->CopyFrom(arg1.status().operation_id());

  arg0->send(call);
}


ACTION_P3(
    SendAcknowledgeOperationStatus, frameworkId, agentId, resourceProviderId)
{
  Call call;
  call.set_type(Call::ACKNOWLEDGE_OPERATION_STATUS);
  call.mutable_framework_id()->CopyFrom(frameworkId);

  Call::AcknowledgeOperationStatus* acknowledge =
    call.mutable_acknowledge_operation_status();

  acknowledge->mutable_agent_id()->CopyFrom(agentId);
  acknowledge->mutable_resource_provider_id()->CopyFrom(resourceProviderId);
  acknowledge->set_uuid(arg1.status().uuid().value());
  acknowledge->mutable_operation_id()->CopyFrom(arg1.status().operation_id());

  arg0->send(call);
}

} // namespace scheduler {

using MockHTTPScheduler = tests::scheduler::MockHTTPScheduler<
    mesos::v1::scheduler::Mesos,
    mesos::v1::scheduler::Event>;

} // namespace v1 {


namespace executor {

// A generic mock HTTP executor to be used in tests with gmock.
template <typename Mesos, typename Event>
class MockHTTPExecutor
{
public:
  MOCK_METHOD1_T(connected, void(Mesos*));
  MOCK_METHOD1_T(disconnected, void(Mesos*));
  MOCK_METHOD2_T(subscribed, void(Mesos*, const typename Event::Subscribed&));
  MOCK_METHOD2_T(launch, void(Mesos*, const typename Event::Launch&));
  MOCK_METHOD2_T(launchGroup, void(Mesos*, const typename Event::LaunchGroup&));
  MOCK_METHOD2_T(kill, void(Mesos*, const typename Event::Kill&));
  MOCK_METHOD2_T(message, void(Mesos*, const typename Event::Message&));
  MOCK_METHOD1_T(shutdown, void(Mesos*));
  MOCK_METHOD2_T(error, void(Mesos*, const typename Event::Error&));
  MOCK_METHOD2_T(acknowledged,
                 void(Mesos*, const typename Event::Acknowledged&));

  void events(Mesos* mesos, std::queue<Event> events)
  {
    while (!events.empty()) {
      Event event = std::move(events.front());
      events.pop();

      switch (event.type()) {
        case Event::SUBSCRIBED:
          subscribed(mesos, event.subscribed());
          break;
        case Event::LAUNCH:
          launch(mesos, event.launch());
          break;
        case Event::LAUNCH_GROUP:
          launchGroup(mesos, event.launch_group());
          break;
        case Event::KILL:
          kill(mesos, event.kill());
          break;
        case Event::ACKNOWLEDGED:
          acknowledged(mesos, event.acknowledged());
          break;
        case Event::MESSAGE:
          message(mesos, event.message());
          break;
        case Event::SHUTDOWN:
          shutdown(mesos);
          break;
        case Event::ERROR:
          error(mesos, event.error());
          break;
        case Event::HEARTBEAT:
          break;
        case Event::UNKNOWN:
          LOG(FATAL) << "Received unexpected UNKNOWN event";
          break;
      }
    }
  }
};


// A generic testing interface for the executor library that can be used to
// test the library across various versions.
template <typename Mesos, typename Event>
class TestMesos : public Mesos
{
public:
  TestMesos(
      ContentType contentType,
      const std::shared_ptr<MockHTTPExecutor<Mesos, Event>>& executor,
      const std::map<std::string, std::string>& environment)
    : Mesos(
          contentType,
          lambda::bind(&MockHTTPExecutor<Mesos, Event>::connected,
                       executor,
                       this),
          lambda::bind(&MockHTTPExecutor<Mesos, Event>::disconnected,
                       executor,
                       this),
          lambda::bind(&MockHTTPExecutor<Mesos, Event>::events,
                       executor,
                       this,
                       lambda::_1),
          environment) {}
};

} // namespace executor {


namespace v1 {
namespace executor {

// Alias existing `mesos::v1::executor` classes so that we can easily
// write `v1::executor::` in tests.
using Call = mesos::v1::executor::Call;
using Event = mesos::v1::executor::Event;
using Mesos = mesos::v1::executor::Mesos;


using TestMesos = tests::executor::TestMesos<
    mesos::v1::executor::Mesos,
    mesos::v1::executor::Event>;


// TODO(anand): Move these actions to the `v1::executor` namespace.
ACTION_P2(SendSubscribe, frameworkId, executorId)
{
  mesos::v1::executor::Call call;
  call.mutable_framework_id()->CopyFrom(frameworkId);
  call.mutable_executor_id()->CopyFrom(executorId);

  call.set_type(mesos::v1::executor::Call::SUBSCRIBE);

  call.mutable_subscribe();

  arg0->send(call);
}


ACTION_P3(SendUpdateFromTask, frameworkId, executorId, state)
{
  mesos::v1::TaskStatus status;
  status.mutable_task_id()->CopyFrom(arg1.task().task_id());
  status.mutable_executor_id()->CopyFrom(executorId);
  status.set_state(state);
  status.set_source(mesos::v1::TaskStatus::SOURCE_EXECUTOR);
  status.set_uuid(id::UUID::random().toBytes());

  mesos::v1::executor::Call call;
  call.mutable_framework_id()->CopyFrom(frameworkId);
  call.mutable_executor_id()->CopyFrom(executorId);

  call.set_type(mesos::v1::executor::Call::UPDATE);

  call.mutable_update()->mutable_status()->CopyFrom(status);

  arg0->send(call);
}


ACTION_P3(SendUpdateFromTaskID, frameworkId, executorId, state)
{
  mesos::v1::TaskStatus status;
  status.mutable_task_id()->CopyFrom(arg1.task_id());
  status.mutable_executor_id()->CopyFrom(executorId);
  status.set_state(state);
  status.set_source(mesos::v1::TaskStatus::SOURCE_EXECUTOR);
  status.set_uuid(id::UUID::random().toBytes());

  mesos::v1::executor::Call call;
  call.mutable_framework_id()->CopyFrom(frameworkId);
  call.mutable_executor_id()->CopyFrom(executorId);

  call.set_type(mesos::v1::executor::Call::UPDATE);

  call.mutable_update()->mutable_status()->CopyFrom(status);

  arg0->send(call);
}

} // namespace executor {

using MockHTTPExecutor = tests::executor::MockHTTPExecutor<
    mesos::v1::executor::Mesos,
    mesos::v1::executor::Event>;

} // namespace v1 {


namespace resource_provider {

template <
    typename Event,
    typename Call,
    typename Driver,
    typename ResourceProviderInfo,
    typename ResourceProviderID,
    typename Resource,
    typename Resources,
    typename OperationState,
    typename Operation>
class TestResourceProviderProcess :
  public process::Process<TestResourceProviderProcess<
      Event,
      Call,
      Driver,
      ResourceProviderInfo,
      ResourceProviderID,
      Resource,
      Resources,
      OperationState,
      Operation>>
{
public:
  TestResourceProviderProcess(
      const ResourceProviderInfo& _info,
      const Option<Resources>& _resources = None())
    : process::ProcessBase(process::ID::generate("test-resource-provider")),
      info(_info),
      resources(_resources)
  {
    auto self = this->self();

    ON_CALL(*this, connected()).WillByDefault(Invoke([self]() {
      dispatch(self, &TestResourceProviderProcess::connectedDefault);
    }));
    EXPECT_CALL(*this, connected()).WillRepeatedly(DoDefault());

    ON_CALL(*this, subscribed(_))
      .WillByDefault(
          Invoke([self](const typename Event::Subscribed& subscribed) {
            dispatch(
                self,
                &TestResourceProviderProcess::subscribedDefault,
                subscribed);
          }));
    EXPECT_CALL(*this, subscribed(_)).WillRepeatedly(DoDefault());

    ON_CALL(*this, applyOperation(_))
      .WillByDefault(
          Invoke([self](const typename Event::ApplyOperation& operation) {
            dispatch(
                self,
                &TestResourceProviderProcess::operationDefault,
                operation);
          }));
    EXPECT_CALL(*this, applyOperation(_)).WillRepeatedly(DoDefault());

    ON_CALL(*this, publishResources(_))
      .WillByDefault(
          Invoke([self](const typename Event::PublishResources& publish) {
            dispatch(
                self,
                &TestResourceProviderProcess::publishDefault,
                publish);
          }));
    EXPECT_CALL(*this, publishResources(_)).WillRepeatedly(DoDefault());

    ON_CALL(*this, teardown()).WillByDefault(Invoke([self]() {
      dispatch(self, &TestResourceProviderProcess::teardownDefault);
    }));
    EXPECT_CALL(*this, teardown()).WillRepeatedly(DoDefault());
  }

  MOCK_METHOD0_T(connected, void());
  MOCK_METHOD0_T(disconnected, void());
  MOCK_METHOD1_T(subscribed, void(const typename Event::Subscribed&));
  MOCK_METHOD1_T(applyOperation, void(const typename Event::ApplyOperation&));
  MOCK_METHOD1_T(
      publishResources,
      void(const typename Event::PublishResources&));
  MOCK_METHOD1_T(
      acknowledgeOperationStatus,
      void(const typename Event::AcknowledgeOperationStatus&));
  MOCK_METHOD1_T(
      reconcileOperations,
      void(const typename Event::ReconcileOperations&));
  MOCK_METHOD0_T(teardown, void());

  void events(std::queue<Event> events)
  {
    while (!events.empty()) {
      Event event = events.front();

      events.pop();

      switch (event.type()) {
        case Event::SUBSCRIBED:
          subscribed(event.subscribed());
          break;
        case Event::APPLY_OPERATION:
          applyOperation(event.apply_operation());
          break;
        case Event::PUBLISH_RESOURCES:
          publishResources(event.publish_resources());
          break;
        case Event::ACKNOWLEDGE_OPERATION_STATUS:
          acknowledgeOperationStatus(event.acknowledge_operation_status());
          break;
        case Event::RECONCILE_OPERATIONS:
          reconcileOperations(event.reconcile_operations());
          break;
        case Event::TEARDOWN:
          teardown();
          break;
        case Event::UNKNOWN:
          LOG(FATAL) << "Received unexpected UNKNOWN event";
          break;
      }
    }
  }

  process::Future<Nothing> send(const Call& call)
  {
    if (driver != nullptr) {
      return driver->send(call);
    } else {
      return process::Failure("Cannot send call since driver is torn down");
    }
  }

  void start(
      process::Owned<mesos::internal::EndpointDetector> detector,
      ContentType contentType)
  {
    process::Future<Option<std::string>> token = None();

#ifdef USE_SSL_SOCKET
    mesos::authentication::executor::JWTSecretGenerator secretGenerator(
        DEFAULT_JWT_SECRET_KEY);

    // For resource provider authentication the chosen claims don't matter,
    // only the signature has to be valid.
    // TODO(nfnt): Revisit this once there's authorization of resource provider
    // API calls.
    hashmap<std::string, std::string> claims;
    claims["foo"] = "bar";

    process::http::authentication::Principal principal(None(), claims);

    process::Future<Secret> secret = secretGenerator.generate(principal);

    token = secretGenerator.generate(principal).then(
        [](const Secret& secret) -> Option<std::string> {
          return secret.value().data();
        });
#endif // USE_SSL_SOCKET

    // TODO(bbannier): Remove the `shared_ptr` once we get C++14.
    auto detector_ =
      std::make_shared<process::Owned<EndpointDetector>>(std::move(detector));

    token.then(defer(this->self(), [=](const Option<std::string>& token) {
      driver.reset(new Driver(
          std::move(*detector_),
          contentType,
          process::defer(this->self(), &TestResourceProviderProcess::connected),
          process::defer(
              this->self(), &TestResourceProviderProcess::disconnected),
          process::defer(
              this->self(), &TestResourceProviderProcess::events, lambda::_1),
          token));

      driver->start();

      return Nothing();
    }));
  }

  void stop()
  {
    driver.reset();
  }

  void connectedDefault()
  {
    Call call;
    call.set_type(Call::SUBSCRIBE);
    call.mutable_subscribe()->mutable_resource_provider_info()->CopyFrom(info);

    send(call)
      .onFailed([](const std::string& failure) {
        LOG(INFO) << "Failed to send call: " << failure;
      });
  }

  void subscribedDefault(const typename Event::Subscribed& subscribed)
  {
    info.mutable_id()->CopyFrom(subscribed.provider_id());

    providerId.set(subscribed.provider_id());

    if (resources.isSome()) {
      Resources injected;

      foreach (Resource resource, resources.get()) {
        resource.mutable_provider_id()->CopyFrom(info.id());
        injected += resource;
      }

      Call call;
      call.set_type(Call::UPDATE_STATE);
      call.mutable_resource_provider_id()->CopyFrom(info.id());

      typename Call::UpdateState* update = call.mutable_update_state();
      update->mutable_resources()->CopyFrom(injected);
      update->mutable_resource_version_uuid()->set_value(
          id::UUID::random().toBytes());

      send(call)
        .onFailed([](const std::string& failure) {
          LOG(INFO) << "Failed to send call: " << failure;
        });
    }
  }

  void operationDefault(const typename Event::ApplyOperation& operation)
  {
    CHECK(info.has_id());

    Call call;
    call.set_type(Call::UPDATE_OPERATION_STATUS);
    call.mutable_resource_provider_id()->CopyFrom(info.id());

    typename Call::UpdateOperationStatus* update =
      call.mutable_update_operation_status();
    update->mutable_framework_id()->CopyFrom(operation.framework_id());
    update->mutable_operation_uuid()->CopyFrom(operation.operation_uuid());

    update->mutable_status()->set_state(
        OperationState::OPERATION_FINISHED);

    switch (operation.info().type()) {
      case Operation::LAUNCH:
      case Operation::LAUNCH_GROUP:
        break;
      case Operation::RESERVE:
        break;
      case Operation::UNRESERVE:
        break;
      case Operation::CREATE:
        break;
      case Operation::DESTROY:
        break;
      // TODO(zhitao): Implement default operation for `GROW_VOLUME` and
      // `SHRINK_VOLUME` on mocked resource provider.
      case Operation::GROW_VOLUME:
        break;
      case Operation::SHRINK_VOLUME:
        break;
      case Operation::CREATE_DISK:
        update->mutable_status()->add_converted_resources()->CopyFrom(
            operation.info().create_disk().source());
        update->mutable_status()
          ->mutable_converted_resources(0)
          ->mutable_disk()
          ->mutable_source()
          ->set_type(operation.info().create_disk().target_type());
        if (operation.info().create_disk().has_target_profile()) {
          update->mutable_status()
            ->mutable_converted_resources(0)
            ->mutable_disk()
            ->mutable_source()
            ->set_profile(operation.info().create_disk().target_profile());
        }
        break;
      case Operation::DESTROY_DISK:
        update->mutable_status()->add_converted_resources()->CopyFrom(
            operation.info().destroy_disk().source());
        update->mutable_status()
          ->mutable_converted_resources(0)
          ->mutable_disk()
          ->mutable_source()
          ->set_type(Resource::DiskInfo::Source::RAW);
        break;
      case Operation::UNKNOWN:
        break;
    }

    update->mutable_status()->mutable_uuid()->set_value(
        id::UUID::random().toString());

    update->mutable_status()->mutable_resource_provider_id()->CopyFrom(
        info.id());

    update->mutable_latest_status()->CopyFrom(update->status());

    send(call)
      .onFailed([](const std::string& failure) {
        LOG(INFO) << "Failed to send call: " << failure;
      });
  }

  void publishDefault(const typename Event::PublishResources& publish)
  {
    CHECK(info.has_id());

    Call call;
    call.set_type(Call::UPDATE_PUBLISH_RESOURCES_STATUS);
    call.mutable_resource_provider_id()->CopyFrom(info.id());

    typename Call::UpdatePublishResourcesStatus* update =
      call.mutable_update_publish_resources_status();
    update->mutable_uuid()->CopyFrom(publish.uuid());
    update->set_status(Call::UpdatePublishResourcesStatus::OK);

    send(call)
      .onFailed([](const std::string& failure) {
        LOG(INFO) << "Failed to send call: " << failure;
      });
  }

  void teardownDefault() {}

  process::Future<ResourceProviderID> id() const { return providerId.future(); }

private:
  ResourceProviderInfo info;

  Option<Resources> resources;
  std::unique_ptr<Driver> driver;

  process::Promise<ResourceProviderID> providerId;
};

template <
    typename Event,
    typename Call,
    typename Driver,
    typename ResourceProviderInfo,
    typename ResourceProviderID,
    typename Resource,
    typename Resources,
    typename OperationState,
    typename Operation>
class TestResourceProvider
{
public:
  TestResourceProvider(
      const ResourceProviderInfo& _info,
      const Option<Resources>& _resources = None())
    : process(new TestResourceProviderProcessT(_info, _resources))
  {
    process::spawn(*process);
  }

  ~TestResourceProvider()
  {
    process::terminate(*process);
    process::wait(*process);
  }

  void start(
      process::Owned<mesos::internal::EndpointDetector> detector,
      ContentType contentType)
  {
    process::dispatch(
        *process,
        &TestResourceProviderProcessT::start,
        std::move(detector),
        contentType);
  }

  process::Future<Nothing> send(const Call& call)
  {
    return process::dispatch(
        *process, &TestResourceProviderProcessT::send, call);
  }

  // Made public for mocking.
  using TestResourceProviderProcessT = TestResourceProviderProcess<
      mesos::v1::resource_provider::Event,
      mesos::v1::resource_provider::Call,
      mesos::v1::resource_provider::Driver,
      mesos::v1::ResourceProviderInfo,
      mesos::v1::ResourceProviderID,
      mesos::v1::Resource,
      mesos::v1::Resources,
      mesos::v1::OperationState,
      mesos::v1::Offer::Operation>;

  std::unique_ptr<TestResourceProviderProcessT> process;
};

inline process::Owned<EndpointDetector> createEndpointDetector(
    const process::UPID& pid)
{
  // Start and register a resource provider.
  std::string scheme = "http";

#ifdef USE_SSL_SOCKET
  if (process::network::openssl::flags().enabled) {
    scheme = "https";
  }
#endif

  process::http::URL url(
      scheme,
      pid.address.ip,
      pid.address.port,
      pid.id + "/api/v1/resource_provider");

  return process::Owned<EndpointDetector>(new ConstantEndpointDetector(url));
}

} // namespace resource_provider {


namespace v1 {
namespace resource_provider {

// Alias existing `mesos::v1::resource_provider` classes so that we can easily
// write `v1::resource_provider::` in tests.
using Call = mesos::v1::resource_provider::Call;
using Event = mesos::v1::resource_provider::Event;

} // namespace resource_provider {

using TestResourceProviderProcess =
  tests::resource_provider::TestResourceProviderProcess<
      mesos::v1::resource_provider::Event,
      mesos::v1::resource_provider::Call,
      mesos::v1::resource_provider::Driver,
      mesos::v1::ResourceProviderInfo,
      mesos::v1::ResourceProviderID,
      mesos::v1::Resource,
      mesos::v1::Resources,
      mesos::v1::OperationState,
      mesos::v1::Offer::Operation>;

using TestResourceProvider = tests::resource_provider::TestResourceProvider<
    mesos::v1::resource_provider::Event,
    mesos::v1::resource_provider::Call,
    mesos::v1::resource_provider::Driver,
    mesos::v1::ResourceProviderInfo,
    mesos::v1::ResourceProviderID,
    mesos::v1::Resource,
    mesos::v1::Resources,
    mesos::v1::OperationState,
    mesos::v1::Offer::Operation>;

} // namespace v1 {


// Definition of a MockAuthorizer that can be used in tests with gmock.
class MockAuthorizer : public Authorizer
{
public:
  MockAuthorizer();
  ~MockAuthorizer() override;

  MOCK_METHOD1(
      authorized, process::Future<bool>(const authorization::Request& request));

  MOCK_METHOD2(
      getApprover,
      process::Future<std::shared_ptr<const ObjectApprover>>(
          const Option<authorization::Subject>& subject,
          const authorization::Action& action));
};


// Definition of a MockGarbageCollector that can be used in tests with gmock.
class MockGarbageCollector : public slave::GarbageCollector
{
public:
  explicit MockGarbageCollector(const std::string& workDir);
  ~MockGarbageCollector() override;

  // The default action is to always return `true`.
  MOCK_METHOD1(unschedule, process::Future<bool>(const std::string& path));
};


class MockSecretGenerator : public SecretGenerator
{
public:
  MockSecretGenerator() = default;
  ~MockSecretGenerator() override = default;

  MOCK_METHOD1(generate, process::Future<Secret>(
      const process::http::authentication::Principal& principal));
};


ACTION_P(SendStatusUpdateFromTask, state)
{
  TaskStatus status;
  status.mutable_task_id()->MergeFrom(arg1.task_id());
  status.set_state(state);
  arg0->sendStatusUpdate(status);
}


ACTION_P(SendStatusUpdateFromTaskID, state)
{
  TaskStatus status;
  status.mutable_task_id()->MergeFrom(arg1);
  status.set_state(state);
  arg0->sendStatusUpdate(status);
}


ACTION_P(SendFrameworkMessage, data)
{
  arg0->sendFrameworkMessage(data);
}


#define FUTURE_PROTOBUF(message, from, to)              \
  FutureProtobuf(message, from, to)


#define DROP_PROTOBUF(message, from, to)              \
  FutureProtobuf(message, from, to, true)


#define DROP_PROTOBUFS(message, from, to)              \
  DropProtobufs(message, from, to)


#define EXPECT_NO_FUTURE_PROTOBUFS(message, from, to)              \
  ExpectNoFutureProtobufs(message, from, to)


#define FUTURE_HTTP_PROTOBUF(message, path, contentType)   \
  FutureHttp(message, path, contentType)


#define DROP_HTTP_PROTOBUF(message, path, contentType)     \
  FutureHttp(message, path, contentType, true)


#define DROP_HTTP_PROTOBUFS(message, path, contentType)    \
  DropHttpProtobufs(message, path, contentType)


#define EXPECT_NO_FUTURE_HTTP_PROTOBUFS(message, path, contentType)  \
  ExpectNoFutureHttpProtobufs(message, path, contentType)


// These are specialized versions of {FUTURE,DROP}_PROTOBUF that
// capture a scheduler/executor Call protobuf of the given 'type'.
// Note that we name methods as '*ProtobufUnion()' because these could
// be reused for macros that capture any protobufs that are described
// using the standard protocol buffer "union" trick (e.g.,
// FUTURE_EVENT to capture scheduler::Event), see
// https://developers.google.com/protocol-buffers/docs/techniques#union.

#define FUTURE_CALL(message, unionType, from, to)              \
  FutureUnionProtobuf(message, unionType, from, to)


#define DROP_CALL(message, unionType, from, to)                \
  FutureUnionProtobuf(message, unionType, from, to, true)


#define DROP_CALLS(message, unionType, from, to)               \
  DropUnionProtobufs(message, unionType, from, to)


#define EXPECT_NO_FUTURE_CALLS(message, unionType, from, to)   \
  ExpectNoFutureUnionProtobufs(message, unionType, from, to)


#define FUTURE_CALL_MESSAGE(message, unionType, from, to)          \
  process::FutureUnionMessage(message, unionType, from, to)


#define DROP_CALL_MESSAGE(message, unionType, from, to)            \
  process::FutureUnionMessage(message, unionType, from, to, true)


#define FUTURE_HTTP_CALL(message, unionType, path, contentType)  \
  FutureUnionHttp(message, unionType, path, contentType)


#define DROP_HTTP_CALL(message, unionType, path, contentType)    \
  FutureUnionHttp(message, unionType, path, contentType, true)


#define DROP_HTTP_CALLS(message, unionType, path, contentType)   \
  DropUnionHttpProtobufs(message, unionType, path, contentType)


#define EXPECT_NO_FUTURE_HTTP_CALLS(message, unionType, path, contentType)   \
  ExpectNoFutureUnionHttpProtobufs(message, unionType, path, contentType)


// Forward declaration.
template <typename T>
T _FutureProtobuf(const process::Message& message);


template <typename T, typename From, typename To>
process::Future<T> FutureProtobuf(T t, From from, To to, bool drop = false)
{
  // Help debugging by adding some "type constraints".
  { google::protobuf::Message* m = &t; (void) m; }

  return process::FutureMessage(testing::Eq(t.GetTypeName()), from, to, drop)
    .then(lambda::bind(&_FutureProtobuf<T>, lambda::_1));
}


template <typename Message, typename UnionType, typename From, typename To>
process::Future<Message> FutureUnionProtobuf(
    Message message, UnionType unionType, From from, To to, bool drop = false)
{
  // Help debugging by adding some "type constraints".
  { google::protobuf::Message* m = &message; (void) m; }

  return process::FutureUnionMessage(message, unionType, from, to, drop)
    .then(lambda::bind(&_FutureProtobuf<Message>, lambda::_1));
}


template <typename Message, typename Path>
process::Future<Message> FutureHttp(
    Message message,
    Path path,
    ContentType contentType,
    bool drop = false)
{
  // Help debugging by adding some "type constraints".
  { google::protobuf::Message* m = &message; (void) m; }

  auto deserializer =
    lambda::bind(&deserialize<Message>, contentType, lambda::_1);

  return process::FutureHttpRequest(message, path, deserializer, drop)
    .then([deserializer](const process::http::Request& request) {
      return deserializer(request.body).get();
    });
}


template <typename Message, typename UnionType, typename Path>
process::Future<Message> FutureUnionHttp(
    Message message,
    UnionType unionType,
    Path path,
    ContentType contentType,
    bool drop = false)
{
  // Help debugging by adding some "type constraints".
  { google::protobuf::Message* m = &message; (void) m; }

  auto deserializer =
    lambda::bind(&deserialize<Message>, contentType, lambda::_1);

  return process::FutureUnionHttpRequest(
      message, unionType, path, deserializer, drop)
    .then([deserializer](const process::http::Request& request) {
      return deserializer(request.body).get();
    });
}


template <typename T>
T _FutureProtobuf(const process::Message& message)
{
  T t;
  t.ParseFromString(message.body);
  return t;
}


template <typename T, typename From, typename To>
void DropProtobufs(T t, From from, To to)
{
  // Help debugging by adding some "type constraints".
  { google::protobuf::Message* m = &t; (void) m; }

  process::DropMessages(testing::Eq(t.GetTypeName()), from, to);
}


template <typename Message, typename UnionType, typename From, typename To>
void DropUnionProtobufs(Message message, UnionType unionType, From from, To to)
{
  // Help debugging by adding some "type constraints".
  { google::protobuf::Message* m = &message; (void) m; }

  process::DropUnionMessages(message, unionType, from, to);
}


template <typename Message, typename Path>
void DropHttpProtobufs(
    Message message,
    Path path,
    ContentType contentType,
    bool drop = false)
{
  // Help debugging by adding some "type constraints".
  { google::protobuf::Message* m = &message; (void) m; }

  auto deserializer =
    lambda::bind(&deserialize<Message>, contentType, lambda::_1);

  process::DropHttpRequests(message, path, deserializer);
}


template <typename Message, typename UnionType, typename Path>
void DropUnionHttpProtobufs(
    Message message,
    UnionType unionType,
    Path path,
    ContentType contentType,
    bool drop = false)
{
  // Help debugging by adding some "type constraints".
  { google::protobuf::Message* m = &message; (void) m; }

  auto deserializer =
    lambda::bind(&deserialize<Message>, contentType, lambda::_1);

  process::DropUnionHttpRequests(message, unionType, path, deserializer);
}


template <typename T, typename From, typename To>
void ExpectNoFutureProtobufs(T t, From from, To to)
{
  // Help debugging by adding some "type constraints".
  { google::protobuf::Message* m = &t; (void) m; }

  process::ExpectNoFutureMessages(testing::Eq(t.GetTypeName()), from, to);
}


template <typename Message, typename UnionType, typename From, typename To>
void ExpectNoFutureUnionProtobufs(
    Message message, UnionType unionType, From from, To to)
{
  // Help debugging by adding some "type constraints".
  { google::protobuf::Message* m = &message; (void) m; }

  process::ExpectNoFutureUnionMessages(message, unionType, from, to);
}


template <typename Message, typename Path>
void ExpectNoFutureHttpProtobufs(
    Message message,
    Path path,
    ContentType contentType,
    bool drop = false)
{
  // Help debugging by adding some "type constraints".
  { google::protobuf::Message* m = &message; (void) m; }

  auto deserializer =
    lambda::bind(&deserialize<Message>, contentType, lambda::_1);

  process::ExpectNoFutureHttpRequests(message, path, deserializer);
}


template <typename Message, typename UnionType, typename Path>
void ExpectNoFutureUnionHttpProtobufs(
    Message message,
    UnionType unionType,
    Path path,
    ContentType contentType,
    bool drop = false)
{
  // Help debugging by adding some "type constraints".
  { google::protobuf::Message* m = &message; (void) m; }

  auto deserializer =
    lambda::bind(&deserialize<Message>, contentType, lambda::_1);

  process::ExpectNoFutureUnionHttpRequests(
      message, unionType, path, deserializer);
}


// This matcher is used to match a vector of resource offers that
// contains an offer having any resource that passes the filter.
MATCHER_P(OffersHaveAnyResource, filter, "")
{
  foreach (const Offer& offer, arg) {
    foreach (const Resource& resource, offer.resources()) {
      if (filter(resource)) {
        return true;
      }
    }
  }

  return false;
}


// This matcher is used to match a vector of resource offers that
// contains an offer having the specified resource.
MATCHER_P(OffersHaveResource, resource, "")
{
  foreach (const Offer& offer, arg) {
    Resources resources = offer.resources();

    // If `resource` is not allocated, we are matching offers against
    // resources constructed from scratch, so we strip off allocations.
    if (!resource.has_allocation_info()) {
      resources.unallocate();
    }

    if (resources.contains(resource)) {
      return true;
    }
  }

  return false;
}


// This matcher is used to match the task id of a `TaskStatus` message.
MATCHER_P(TaskStatusTaskIdEq, taskId, "")
{
  return arg.task_id() == taskId;
}


// This matcher is used to match the state of a `TaskStatus` message.
MATCHER_P(TaskStatusStateEq, taskState, "")
{
  return arg.state() == taskState;
}


// This matcher is used to match the task id of an `Event.update.status`
// message.
MATCHER_P(TaskStatusUpdateTaskIdEq, taskId, "")
{
  return arg.status().task_id() == taskId;
}


// This matcher is used to match the state of an `Event.update.status`
// message.
MATCHER_P(TaskStatusUpdateStateEq, taskState, "")
{
  return arg.status().state() == taskState;
}


// This matcher is used to match the task id of
// `authorization::Request.Object.TaskInfo`.
MATCHER_P(AuthorizationRequestHasTaskID, taskId, "")
{
  if (!arg.has_object()) {
    return false;
  }

  if (!arg.object().has_task_info()) {
    return false;
  }

  return arg.object().task_info().task_id() == taskId;
}


// This matcher is used to match the task id of `Option<TaskInfo>`.
MATCHER_P(OptionTaskHasTaskID, taskId, "")
{
  return arg.isNone() ? false : arg->task_id() == taskId;
}


// This matcher is used to match an `Option<TaskGroupInfo>` which contains a
// task with the specified task id.
MATCHER_P(OptionTaskGroupHasTaskID, taskId, "")
{
  if (arg.isNone()) {
    return false;
  }

  foreach(const TaskInfo& taskInfo, arg->tasks()) {
    if (taskInfo.task_id() == taskId) {
      return true;
    }
  }

  return false;
}


struct ParamExecutorType
{
public:
  struct Printer
  {
    std::string operator()(
        const ::testing::TestParamInfo<ParamExecutorType>& info) const
    {
      switch (info.param.type) {
        case COMMAND:
          return "CommandExecutor";
        case DEFAULT:
          return "DefaultExecutor";
        default:
          UNREACHABLE();
      }
    }
  };

  static ParamExecutorType commandExecutor()
  {
    return ParamExecutorType(COMMAND);
  }

  static ParamExecutorType defaultExecutor()
  {
    return ParamExecutorType(DEFAULT);
  }

  bool isCommandExecutor() const { return type == COMMAND; }
  bool isDefaultExecutor() const { return type == DEFAULT; }

private:
  enum Type
  {
    COMMAND,
    DEFAULT
  };

  ParamExecutorType(Type _type) : type(_type) {}

  Type type;
};


struct ParamDiskQuota
{
  enum Type
  {
    SANDBOX,
    ROOTFS,
  };

  struct Printer
  {
    std::string operator()(
        const ::testing::TestParamInfo<ParamDiskQuota::Type>& info) const;
  };

  static std::vector<Type> parameters();
};

} // namespace tests {
} // namespace internal {
} // namespace mesos {

#endif // __TESTS_MESOS_HPP__
