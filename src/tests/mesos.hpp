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
#include <vector>

#include <gmock/gmock.h>

#include <mesos/executor.hpp>
#include <mesos/scheduler.hpp>

#include <mesos/v1/executor.hpp>
#include <mesos/v1/resources.hpp>
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
#include <process/owned.hpp>
#include <process/pid.hpp>
#include <process/process.hpp>
#include <process/queue.hpp>

#include <process/ssl/gtest.hpp>

#include <stout/bytes.hpp>
#include <stout/foreach.hpp>
#include <stout/gtest.hpp>
#include <stout/lambda.hpp>
#include <stout/none.hpp>
#include <stout/option.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>
#include <stout/uuid.hpp>

#include "common/http.hpp"

#include "messages/messages.hpp" // For google::protobuf::Message.

#include "master/master.hpp"

#include "sched/constants.hpp"

#include "slave/constants.hpp"
#include "slave/slave.hpp"

#include "slave/containerizer/containerizer.hpp"
#include "slave/containerizer/fetcher.hpp"

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
constexpr char DEFAULT_EXECUTOR_SECRET_KEY[] =
  "72kUKUFtghAjNbIOvLzfF2RxNBfeM64Bri8g9WhpyaunwqRB/yozHAqSnyHbddAV"
  "PcWRQlrJAt871oWgSH+n52vMZ3aVI+AFMzXSo8+sUfMk83IGp0WJefhzeQsjDlGH"
  "GYQgCAuGim0BE2X5U+lEue8s697uQpAO8L/FFRuDH2s";


// Forward declarations.
class MockExecutor;


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

  // TODO(bmahler): Consider adding a builder style interface, e.g.
  //
  // Try<PID<Slave>> slave =
  //   Slave().With(flags)
  //          .With(executor)
  //          .With(containerizer)
  //          .With(detector)
  //          .With(gc)
  //          .Start();
  //
  // Or options:
  //
  // Injections injections;
  // injections.executor = executor;
  // injections.containerizer = containerizer;
  // injections.detector = detector;
  // injections.gc = gc;
  // Try<PID<Slave>> slave = StartSlave(injections);

  // Starts a slave with the specified detector and flags.
  virtual Try<process::Owned<cluster::Slave>> StartSlave(
      mesos::master::detector::MasterDetector* detector,
      const Option<slave::Flags>& flags = None());

  // Starts a slave with the specified detector, containerizer, and flags.
  virtual Try<process::Owned<cluster::Slave>> StartSlave(
      mesos::master::detector::MasterDetector* detector,
      slave::Containerizer* containerizer,
      const Option<slave::Flags>& flags = None());

  // Starts a slave with the specified detector, id, and flags.
  virtual Try<process::Owned<cluster::Slave>> StartSlave(
      mesos::master::detector::MasterDetector* detector,
      const std::string& id,
      const Option<slave::Flags>& flags = None());

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
      const Option<slave::Flags>& flags = None());

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
      const Option<slave::Flags>& flags = None());

  // Starts a slave with the specified detector, authorizer, and flags.
  virtual Try<process::Owned<cluster::Slave>> StartSlave(
      mesos::master::detector::MasterDetector* detector,
      mesos::Authorizer* authorizer,
      const Option<slave::Flags>& flags = None());

  // Starts a slave with the specified detector, containerizer, authorizer,
  // and flags.
  virtual Try<process::Owned<cluster::Slave>> StartSlave(
      mesos::master::detector::MasterDetector* detector,
      slave::Containerizer* containerizer,
      mesos::Authorizer* authorizer,
      const Option<slave::Flags>& flags = None());

  Option<zookeeper::URL> zookeeperUrl;

  const std::string defaultAgentResourcesString{
    "cpus:2;gpus:0;mem:1024;disk:1024;ports:[31000-32000]"};
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
  virtual slave::Flags CreateSlaveFlags();
  virtual void SetUp();
  virtual void TearDown();

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

  virtual void SetUp()
  {
    MesosTest::SetUp();
    server->startNetwork();
  }

  virtual void TearDown()
  {
    server->shutdownNetwork();
    MesosTest::TearDown();
  }

protected:
  MesosZooKeeperTest() : MesosTest(url) {}

  virtual master::Flags CreateMasterFlags()
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
using mesos::v1::Resource;
using mesos::v1::Resources;
using mesos::v1::TaskID;
using mesos::v1::TaskInfo;
using mesos::v1::TaskGroupInfo;
using mesos::v1::TaskState;
using mesos::v1::TaskStatus;
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
template <typename TExecutorInfo, typename TResources, typename TCommandInfo>
inline TExecutorInfo createExecutorInfo(
    const std::string& executorId,
    const Option<TCommandInfo>& command = None(),
    const Option<std::string>& resources = None(),
    const Option<typename TExecutorInfo::Type>& type = None())
{
  TExecutorInfo executor;
  executor.mutable_executor_id()->set_value(executorId);
  if (command.isSome()) {
    executor.mutable_command()->CopyFrom(command.get());
  }
  if (resources.isSome()) {
    executor.mutable_resources()->CopyFrom(
        TResources::parse(resources.get()).get());
  }
  if (type.isSome()) {
    executor.set_type(type.get());
  }
  return executor;
}


template <typename TExecutorInfo, typename TResources, typename TCommandInfo>
inline TExecutorInfo createExecutorInfo(
    const std::string& executorId,
    const std::string& command,
    const Option<std::string>& resources = None(),
    const Option<typename TExecutorInfo::Type>& type = None())
{
  TCommandInfo commandInfo;
  commandInfo.set_value(command);
  return createExecutorInfo<TExecutorInfo, TResources, TCommandInfo>(
      executorId, commandInfo, resources, type);
}


template <typename TExecutorInfo, typename TResources, typename TCommandInfo>
inline TExecutorInfo createExecutorInfo(
    const std::string& executorId,
    const char* command,
    const Option<std::string>& resources = None(),
    const Option<typename TExecutorInfo::Type>& type = None())
{
  return createExecutorInfo<TExecutorInfo, TResources, TCommandInfo>(
      executorId,
      std::string(command),
      resources);
}


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


template <typename TImage>
inline TImage createDockerImage(const std::string& imageName)
{
  TImage image;
  image.set_type(TImage::DOCKER);
  image.mutable_docker()->set_name(imageName);
  return image;
}


template <typename TVolume>
inline TVolume createVolumeFromHostPath(
    const std::string& containerPath,
    const std::string& hostPath,
    const typename TVolume::Mode& mode)
{
  TVolume volume;
  volume.set_container_path(containerPath);
  volume.set_host_path(hostPath);
  volume.set_mode(mode);
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
    typename TOffer>
inline TTaskInfo createTask(
    const TSlaveID& slaveId,
    const TResources& resources,
    const TCommandInfo& command,
    const Option<TExecutorID>& executorId = None(),
    const std::string& name = "test-task",
    const std::string& id = UUID::random().toString())
{
  TTaskInfo task;
  task.set_name(name);
  task.mutable_task_id()->set_value(id);
  setAgentID(&task, slaveId);
  task.mutable_resources()->CopyFrom(resources);
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
    typename TOffer>
inline TTaskInfo createTask(
    const TSlaveID& slaveId,
    const TResources& resources,
    const std::string& command,
    const Option<TExecutorID>& executorId = None(),
    const std::string& name = "test-task",
    const std::string& id = UUID::random().toString())
{
  return createTask<
      TTaskInfo,
      TExecutorID,
      TSlaveID,
      TResources,
      TExecutorInfo,
      TCommandInfo,
      TOffer>(
          slaveId,
          resources,
          createCommandInfo<TCommandInfo>(command),
          executorId,
          name,
          id);
}


template <
    typename TTaskInfo,
    typename TExecutorID,
    typename TSlaveID,
    typename TResources,
    typename TExecutorInfo,
    typename TCommandInfo,
    typename TOffer>
inline TTaskInfo createTask(
    const TOffer& offer,
    const std::string& command,
    const Option<TExecutorID>& executorId = None(),
    const std::string& name = "test-task",
    const std::string& id = UUID::random().toString())
{
  return createTask<
      TTaskInfo,
      TExecutorID,
      TSlaveID,
      TResources,
      TExecutorInfo,
      TCommandInfo,
      TOffer>(
          offer.slave_id(),
          offer.resources(),
          command,
          executorId,
          name,
          id);
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


template <typename TResource, typename TLabels>
inline typename TResource::ReservationInfo createReservationInfo(
    const Option<std::string>& principal = None(),
    const Option<TLabels>& labels = None())
{
  typename TResource::ReservationInfo info;

  if (principal.isSome()) {
    info.set_principal(principal.get());
  }

  if (labels.isSome()) {
    info.mutable_labels()->CopyFrom(labels.get());
  }

  return info;
}


template <typename TResource, typename TResources>
inline TResource createReservedResource(
    const std::string& name,
    const std::string& value,
    const std::string& role,
    const Option<typename TResource::ReservationInfo>& reservation)
{
  TResource resource = TResources::parse(name, value, role).get();

  if (reservation.isSome()) {
    resource.mutable_reservation()->CopyFrom(reservation.get());
  }

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
    const std::string& root)
{
  typename TResource::DiskInfo::Source source;

  source.set_type(TResource::DiskInfo::Source::PATH);
  source.mutable_path()->set_root(root);

  return source;
}


// Helper for creating a disk source with type `MOUNT`.
template <typename TResource>
inline typename TResource::DiskInfo::Source createDiskSourceMount(
    const std::string& root)
{
  typename TResource::DiskInfo::Source source;

  source.set_type(TResource::DiskInfo::Source::MOUNT);
  source.mutable_mount()->set_root(root);

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
      stringify(size.megabytes()),
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
    volume.mutable_reservation()->set_principal(reservationPrincipal.get());
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
    volume.mutable_reservation()->set_principal(reservationPrincipal.get());
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


// Helpers for creating offer operations.
template <typename TResources, typename TOffer>
inline typename TOffer::Operation RESERVE(const TResources& resources)
{
  typename TOffer::Operation operation;
  operation.set_type(TOffer::Operation::RESERVE);
  operation.mutable_reserve()->mutable_resources()->CopyFrom(resources);
  return operation;
}


template <typename TResources, typename TOffer>
inline typename TOffer::Operation UNRESERVE(const TResources& resources)
{
  typename TOffer::Operation operation;
  operation.set_type(TOffer::Operation::UNRESERVE);
  operation.mutable_unreserve()->mutable_resources()->CopyFrom(resources);
  return operation;
}


template <typename TResources, typename TOffer>
inline typename TOffer::Operation CREATE(const TResources& volumes)
{
  typename TOffer::Operation operation;
  operation.set_type(TOffer::Operation::CREATE);
  operation.mutable_create()->mutable_volumes()->CopyFrom(volumes);
  return operation;
}


template <typename TResources, typename TOffer>
inline typename TOffer::Operation DESTROY(const TResources& volumes)
{
  typename TOffer::Operation operation;
  operation.set_type(TOffer::Operation::DESTROY);
  operation.mutable_destroy()->mutable_volumes()->CopyFrom(volumes);
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
      Resources,
      CommandInfo>(std::forward<Args>(args)...);
}


// We specify the argument to allow brace initialized construction.
inline CommandInfo createCommandInfo(
    const Option<std::string>& value = None(),
    const std::vector<std::string>& arguments = {})
{
  return common::createCommandInfo<CommandInfo>(value, arguments);
}


template <typename... Args>
inline Image createDockerImage(Args&&... args)
{
  return common::createDockerImage<Image>(std::forward<Args>(args)...);
}


template <typename... Args>
inline Volume createVolumeFromHostPath(Args&&... args)
{
  return common::createVolumeFromHostPath<Volume>(std::forward<Args>(args)...);
}


template <typename... Args>
inline Volume createVolumeFromDockerImage(Args&&... args)
{
  return common::createVolumeFromDockerImage<Volume, Image>(
      std::forward<Args>(args)...);
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
      Offer>(std::forward<Args>(args)...);
}


// We specify the argument to allow brace initialized construction.
inline TaskGroupInfo createTaskGroupInfo(const std::vector<TaskInfo>& tasks)
{
  return common::createTaskGroupInfo<TaskGroupInfo, TaskInfo>(tasks);
}


template <typename... Args>
inline Resource::ReservationInfo createReservationInfo(Args&&... args)
{
  return common::createReservationInfo<Resource, Labels>(
      std::forward<Args>(args)...);
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
inline Offer::Operation RESERVE(Args&&... args)
{
  return common::RESERVE<Resources, Offer>(std::forward<Args>(args)...);
}


template <typename... Args>
inline Offer::Operation UNRESERVE(Args&&... args)
{
  return common::UNRESERVE<Resources, Offer>(std::forward<Args>(args)...);
}


template <typename... Args>
inline Offer::Operation CREATE(Args&&... args)
{
  return common::CREATE<Resources, Offer>(std::forward<Args>(args)...);
}


template <typename... Args>
inline Offer::Operation DESTROY(Args&&... args)
{
  return common::DESTROY<Resources, Offer>(std::forward<Args>(args)...);
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
      mesos::v1::Resources,
      mesos::v1::CommandInfo>(std::forward<Args>(args)...);
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
inline mesos::v1::Volume createVolumeFromHostPath(Args&&... args)
{
  return common::createVolumeFromHostPath<mesos::v1::Volume>(
      std::forward<Args>(args)...);
}


template <typename... Args>
inline mesos::v1::Volume createVolumeFromDockerImage(Args&&... args)
{
  return common::createVolumeFromDockerImage<
      mesos::v1::Volume, mesos::v1::Image>(std::forward<Args>(args)...);
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
      mesos::v1::Offer>(std::forward<Args>(args)...);
}


// We specify the argument to allow brace initialized construction.
inline mesos::v1::TaskGroupInfo createTaskGroupInfo(
    const std::vector<mesos::v1::TaskInfo>& tasks)
{
  return common::createTaskGroupInfo<
      mesos::v1::TaskGroupInfo,
      mesos::v1::TaskInfo>(tasks);
}


template <typename... Args>
inline mesos::v1::Resource::ReservationInfo createReservationInfo(
    Args&&... args)
{
  return common::createReservationInfo<mesos::v1::Resource, mesos::v1::Labels>(
      std::forward<Args>(args)...);
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
  return common::RESERVE<mesos::v1::Resources, mesos::v1::Offer>(
      std::forward<Args>(args)...);
}


template <typename... Args>
inline mesos::v1::Offer::Operation UNRESERVE(Args&&... args)
{
  return common::UNRESERVE<mesos::v1::Resources, mesos::v1::Offer>(
      std::forward<Args>(args)...);
}


template <typename... Args>
inline mesos::v1::Offer::Operation CREATE(Args&&... args)
{
  return common::CREATE<mesos::v1::Resources, mesos::v1::Offer>(
      std::forward<Args>(args)...);
}


template <typename... Args>
inline mesos::v1::Offer::Operation DESTROY(Args&&... args)
{
  return common::DESTROY<mesos::v1::Resources, mesos::v1::Offer>(
      std::forward<Args>(args)...);
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
inline mesos::v1::Parameters parameterize(Args&&... args)
{
  return common::parameterize<mesos::v1::Parameters, mesos::v1::Parameter>(
      std::forward<Args>(args)...);
}


inline mesos::v1::scheduler::Call createCallAccept(
    const mesos::v1::FrameworkID& frameworkId,
    const mesos::v1::Offer& offer,
    const std::vector<mesos::v1::Offer::Operation>& operations)
{
  mesos::v1::scheduler::Call call;
  call.set_type(mesos::v1::scheduler::Call::ACCEPT);
  call.mutable_framework_id()->CopyFrom(frameworkId);

  mesos::v1::scheduler::Call::Accept* accept = call.mutable_accept();
  accept->add_offer_ids()->CopyFrom(offer.id());

  foreach (const mesos::v1::Offer::Operation& operation, operations) {
    accept->add_operations()->CopyFrom(operation);
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
  virtual ~MockScheduler();

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
    taskResources.allocate(role);

    int nextTaskId = 0;
    std::vector<TaskInfo> tasks;
    Resources remaining = offer.resources();

    while (remaining.flatten().contains(taskResources) &&
           launched < numTasks) {
      TaskInfo task;
      task.set_name("TestTask");
      task.mutable_task_id()->set_value(stringify(nextTaskId++));
      task.mutable_slave_id()->MergeFrom(offer.slave_id());
      task.mutable_executor()->MergeFrom(executor);

      Option<Resources> resources =
        remaining.find(taskResources.flatten(role).get());

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
  virtual ~MockExecutor();

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
  MOCK_METHOD2_T(message, void(Mesos*, const typename Event::Message&));
  MOCK_METHOD2_T(failure, void(Mesos*, const typename Event::Failure&));
  MOCK_METHOD2_T(error, void(Mesos*, const typename Event::Error&));

  void event(Mesos* mesos, const Event& event)
  {
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
      const std::shared_ptr<MockHTTPScheduler<Mesos, Event>>& _scheduler,
      const Option<std::shared_ptr<mesos::master::detector::MasterDetector>>&
          detector = None())
    : Mesos(
          master,
          contentType,
          // We don't pass the `_scheduler` shared pointer as the library
          // interface expects a `std::function` object.
          lambda::bind(&MockHTTPScheduler<Mesos, Event>::connected,
                       _scheduler.get(),
                       this),
          lambda::bind(&MockHTTPScheduler<Mesos, Event>::disconnected,
                       _scheduler.get(),
                       this),
          lambda::bind(&TestMesos<Mesos, Event>::events,
                       this,
                       lambda::_1),
          v1::DEFAULT_CREDENTIAL,
          detector),
      scheduler(_scheduler) {}

  virtual ~TestMesos()
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

protected:
  void events(std::queue<Event> events)
  {
    while (!events.empty()) {
      Event event = std::move(events.front());
      events.pop();
      scheduler->event(this, event);
    }
  }

private:
  std::shared_ptr<MockHTTPScheduler<Mesos, Event>> scheduler;
};

} // namespace scheduler {


namespace v1 {
namespace scheduler {

using Call = mesos::v1::scheduler::Call;
using Event = mesos::v1::scheduler::Event;
using Mesos = mesos::v1::scheduler::Mesos;


using TestMesos = tests::scheduler::TestMesos<
    mesos::v1::scheduler::Mesos,
    mesos::v1::scheduler::Event>;


ACTION_P(SendSubscribe, frameworkInfo)
{
  Call call;
  call.set_type(Call::SUBSCRIBE);
  call.mutable_subscribe()->mutable_framework_info()->CopyFrom(frameworkInfo);
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

  void event(Mesos* mesos, const Event& event)
  {
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
      case Event::UNKNOWN:
        LOG(FATAL) << "Received unexpected UNKNOWN event";
        break;
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
      const std::shared_ptr<MockHTTPExecutor<Mesos, Event>>& _executor)
    : Mesos(
          contentType,
          lambda::bind(&MockHTTPExecutor<Mesos, Event>::connected,
                       _executor,
                       this),
          lambda::bind(&MockHTTPExecutor<Mesos, Event>::disconnected,
                       _executor,
                       this),
          lambda::bind(&TestMesos<Mesos, Event>::events,
                       this,
                       lambda::_1)),
      executor(_executor) {}

protected:
  void events(std::queue<Event> events)
  {
    while (!events.empty()) {
      Event event = std::move(events.front());
      events.pop();
      executor->event(this, event);
    }
  }

private:
  // TODO(bmahler): This is a shared pointer because the `Mesos`
  // library copies the pointer into callbacks that can execute
  // after `Mesos` is destructed. We can avoid this by ensuring
  // that `~Mesos()` blocks until deferred callbacks are cleared
  // (merely grabbing the `process::Mutex` lock is sufficient).
  // The `Mesos` library can also provide a `Future<Nothing> stop()`
  // to allow callers to wait for all events to be flushed.
  std::shared_ptr<MockHTTPExecutor<Mesos, Event>> executor;
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
  status.set_uuid(UUID::random().toBytes());

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
  status.set_uuid(UUID::random().toBytes());

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


// Definition of a mock FetcherProcess to be used in tests with gmock.
class MockFetcherProcess : public slave::FetcherProcess
{
public:
  MockFetcherProcess();
  virtual ~MockFetcherProcess();

  MOCK_METHOD6(_fetch, process::Future<Nothing>(
      const hashmap<
          CommandInfo::URI,
          Option<process::Future<std::shared_ptr<Cache::Entry>>>>&
        entries,
      const ContainerID& containerId,
      const std::string& sandboxDirectory,
      const std::string& cacheDirectory,
      const Option<std::string>& user,
      const slave::Flags& flags));

  process::Future<Nothing> unmocked__fetch(
      const hashmap<
          CommandInfo::URI,
          Option<process::Future<std::shared_ptr<Cache::Entry>>>>&
        entries,
      const ContainerID& containerId,
      const std::string& sandboxDirectory,
      const std::string& cacheDirectory,
      const Option<std::string>& user,
      const slave::Flags& flags);

  MOCK_METHOD5(run, process::Future<Nothing>(
      const ContainerID& containerId,
      const std::string& sandboxDirectory,
      const Option<std::string>& user,
      const mesos::fetcher::FetcherInfo& info,
      const slave::Flags& flags));

  process::Future<Nothing> unmocked_run(
      const ContainerID& containerId,
      const std::string& sandboxDirectory,
      const Option<std::string>& user,
      const mesos::fetcher::FetcherInfo& info,
      const slave::Flags& flags);
};


// Definition of a MockAuthorizer that can be used in tests with gmock.
class MockAuthorizer : public Authorizer
{
public:
  MockAuthorizer();
  virtual ~MockAuthorizer();

  MOCK_METHOD1(
      authorized, process::Future<bool>(const authorization::Request& request));

  MOCK_METHOD2(
      getObjectApprover, process::Future<process::Owned<ObjectApprover>>(
          const Option<authorization::Subject>& subject,
          const authorization::Action& action));
};


class MockSecretGenerator : public SecretGenerator
{
public:
  MockSecretGenerator() = default;
  virtual ~MockSecretGenerator() = default;

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


// This matcher is used to match the task ids of TaskStatus messages.
// Suppose we set up N futures for LaunchTasks and N futures for StatusUpdates.
// (This is a common pattern). We get into a situation where all StatusUpdates
// are satisfied before the LaunchTasks if the master re-sends StatusUpdates.
// We use this matcher to only satisfy the StatusUpdate future if the
// StatusUpdate came from the corresponding task.
MATCHER_P(TaskStatusEq, task, "") { return arg.task_id() == task.task_id(); }

} // namespace tests {
} // namespace internal {
} // namespace mesos {

#endif // __TESTS_MESOS_HPP__
