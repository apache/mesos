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

#include <utility>

#include <gmock/gmock.h>

#include <mesos/authentication/secret_generator.hpp>

#include <mesos/slave/qos_controller.hpp>
#include <mesos/slave/resource_estimator.hpp>

#include <process/future.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>

#include <stout/option.hpp>

#include "slave/csi_server.hpp"
#include "slave/slave.hpp"
#include "slave/task_status_update_manager.hpp"

#include "tests/mock_slave.hpp"

using mesos::master::detector::MasterDetector;

using mesos::internal::slave::Containerizer;
using mesos::internal::slave::GarbageCollector;
using mesos::internal::slave::TaskStatusUpdateManager;
using mesos::internal::slave::VolumeGidManager;

using mesos::slave::ContainerTermination;
using mesos::slave::ResourceEstimator;
using mesos::slave::QoSController;

using std::list;
using std::string;
using std::vector;

using process::Future;
using process::Owned;
using process::UPID;

using testing::_;
using testing::Invoke;

namespace mesos {
namespace internal {
namespace tests {

MockResourceEstimator::MockResourceEstimator()
{
  ON_CALL(*this, initialize(_))
    .WillByDefault(Return(Nothing()));
  EXPECT_CALL(*this, initialize(_))
    .WillRepeatedly(DoDefault());

  ON_CALL(*this, oversubscribable())
    .WillByDefault(Return(Future<Resources>()));
  EXPECT_CALL(*this, oversubscribable())
    .WillRepeatedly(DoDefault());
}


MockResourceEstimator::~MockResourceEstimator() {}


MockQoSController::MockQoSController()
{
  ON_CALL(*this, initialize(_))
    .WillByDefault(Return(Nothing()));
  EXPECT_CALL(*this, initialize(_))
    .WillRepeatedly(DoDefault());

  ON_CALL(*this, corrections())
    .WillByDefault(
        Return(Future<list<mesos::slave::QoSCorrection>>()));
  EXPECT_CALL(*this, corrections())
    .WillRepeatedly(DoDefault());
}


MockQoSController::~MockQoSController() {}


MockSlave::MockSlave(
    const string& id,
    const slave::Flags& flags,
    MasterDetector* detector,
    Containerizer* containerizer,
    Files* files,
    GarbageCollector* gc,
    TaskStatusUpdateManager* taskStatusUpdateManager,
    ResourceEstimator* resourceEstimator,
    QoSController* qosController,
    SecretGenerator* secretGenerator,
    VolumeGidManager* volumeGidManager,
    PendingFutureTracker* futureTracker,
    Owned<slave::CSIServer>&& csiServer,
    const Option<Authorizer*>& authorizer)
  // It is necessary to explicitly call `ProcessBase` constructor here even
  // though the direct parent `Slave` already does this. This is because
  // `ProcessBase` being a virtual base class, if not being explicitly
  // constructed here by passing `id`, will be constructed implicitly first with
  // a default constructor, resulting in lost argument `id`.
  : ProcessBase(id),
    slave::Slave(
        id,
        flags,
        detector,
        containerizer,
        files,
        gc,
        taskStatusUpdateManager,
        resourceEstimator,
        qosController,
        secretGenerator,
        volumeGidManager,
        futureTracker,
        std::move(csiServer),
#ifndef __WINDOWS__
        None(),
#endif // __WINDOWS__
        authorizer)
{
  // Set up default behaviors, calling the original methods.
  EXPECT_CALL(*this, ___run(_, _, _, _, _, _))
    .WillRepeatedly(Invoke(this, &MockSlave::unmocked____run));
  EXPECT_CALL(*this, runTask(_, _, _, _, _, _, _))
    .WillRepeatedly(Invoke(this, &MockSlave::unmocked_runTask));
  EXPECT_CALL(*this, _run(_, _, _, _, _, _))
    .WillRepeatedly(Invoke(this, &MockSlave::unmocked__run));
  EXPECT_CALL(*this, __run(_, _, _, _, _, _, _))
    .WillRepeatedly(Invoke(this, &MockSlave::unmocked___run));
  EXPECT_CALL(*this, runTaskGroup(_, _, _, _, _, _))
    .WillRepeatedly(Invoke(this, &MockSlave::unmocked_runTaskGroup));
  EXPECT_CALL(*this, killTask(_, _))
    .WillRepeatedly(Invoke(this, &MockSlave::unmocked_killTask));
  EXPECT_CALL(*this, authenticate(_, _))
    .WillRepeatedly(Invoke(this, &MockSlave::unmocked_authenticate));
  EXPECT_CALL(*this, removeFramework(_))
    .WillRepeatedly(Invoke(this, &MockSlave::unmocked_removeFramework));
  EXPECT_CALL(*this, __recover(_))
    .WillRepeatedly(Invoke(this, &MockSlave::unmocked___recover));
  EXPECT_CALL(*this, qosCorrections())
    .WillRepeatedly(Invoke(this, &MockSlave::unmocked_qosCorrections));
  EXPECT_CALL(*this, usage())
    .WillRepeatedly(Invoke(this, &MockSlave::unmocked_usage));
  EXPECT_CALL(*this, executorTerminated(_, _, _))
    .WillRepeatedly(Invoke(this, &MockSlave::unmocked_executorTerminated));
  EXPECT_CALL(*this, shutdownExecutor(_, _, _))
    .WillRepeatedly(Invoke(this, &MockSlave::unmocked_shutdownExecutor));
  EXPECT_CALL(*this, _shutdownExecutor(_, _))
    .WillRepeatedly(Invoke(this, &MockSlave::unmocked__shutdownExecutor));
  EXPECT_CALL(*this, applyOperation(_))
    .WillRepeatedly(Invoke(this, &MockSlave::unmocked_applyOperation));
}


void MockSlave::unmocked____run(
    const Future<Nothing>& future,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const ContainerID& containerId,
    const vector<TaskInfo>& tasks,
    const vector<TaskGroupInfo>& taskGroups)
{
  slave::Slave::___run(
      future,
      frameworkId,
      executorId,
      containerId,
      tasks,
      taskGroups);
}


void MockSlave::unmocked_runTask(
    const UPID& from,
    const FrameworkInfo& frameworkInfo,
    const FrameworkID& frameworkId,
    const UPID& pid,
    const TaskInfo& task,
    const vector<ResourceVersionUUID>& resourceVersionUuids,
    const Option<bool>& launchExecutor)
{
  slave::Slave::runTask(
      from,
      frameworkInfo,
      frameworkInfo.id(),
      pid,
      task,
      resourceVersionUuids,
      launchExecutor);
}


Future<Nothing> MockSlave::unmocked__run(
    const FrameworkInfo& frameworkInfo,
    const ExecutorInfo& executorInfo,
    const Option<TaskInfo>& taskInfo,
    const Option<TaskGroupInfo>& taskGroup,
    const std::vector<ResourceVersionUUID>& resourceVersionUuids,
    const Option<bool>& launchExecutor)
{
  return slave::Slave::_run(
      frameworkInfo,
      executorInfo,
      taskInfo,
      taskGroup,
      resourceVersionUuids,
      launchExecutor);
}


void MockSlave::unmocked___run(
    const FrameworkInfo& frameworkInfo,
    const ExecutorInfo& executorInfo,
    const Option<TaskInfo>& task,
    const Option<TaskGroupInfo>& taskGroup,
    const std::vector<ResourceVersionUUID>& resourceVersionUuids,
    const Option<bool>& launchExecutor,
    bool executorGeneratedForCommandTask)
{
  slave::Slave::__run(
      frameworkInfo,
      executorInfo,
      task,
      taskGroup,
      resourceVersionUuids,
      launchExecutor,
      executorGeneratedForCommandTask);
}


void MockSlave::unmocked_runTaskGroup(
    const UPID& from,
    const FrameworkInfo& frameworkInfo,
    const ExecutorInfo& executorInfo,
    const TaskGroupInfo& taskGroup,
    const vector<ResourceVersionUUID>& resourceVersionUuids,
    const Option<bool>& launchExecutor)
{
  slave::Slave::runTaskGroup(
      from,
      frameworkInfo,
      executorInfo,
      taskGroup,
      resourceVersionUuids,
      launchExecutor);
}


void MockSlave::unmocked_killTask(
    const UPID& from,
    const KillTaskMessage& killTaskMessage)
{
  slave::Slave::killTask(from, killTaskMessage);
}


void MockSlave::unmocked_authenticate(
    Duration minTimeout,
    Duration maxTimeout)
{
  slave::Slave::authenticate(minTimeout, maxTimeout);
}


void MockSlave::unmocked_removeFramework(slave::Framework* framework)
{
  slave::Slave::removeFramework(framework);
}


void MockSlave::unmocked___recover(const Future<Nothing>& future)
{
  slave::Slave::__recover(future);
}


void MockSlave::unmocked_qosCorrections()
{
  slave::Slave::qosCorrections();
}


Future<ResourceUsage> MockSlave::unmocked_usage()
{
  return slave::Slave::usage();
}


void MockSlave::unmocked_executorTerminated(
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const Future<Option<ContainerTermination>>& termination)
{
  slave::Slave::executorTerminated(frameworkId, executorId, termination);
}


void MockSlave::unmocked_shutdownExecutor(
    const UPID& from,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId)
{
  slave::Slave::shutdownExecutor(from, frameworkId, executorId);
}


void MockSlave::unmocked__shutdownExecutor(
    slave::Framework* framework,
    slave::Executor* executor)
{
  slave::Slave::_shutdownExecutor(framework, executor);
}


void MockSlave::unmocked_applyOperation(const ApplyOperationMessage& message)
{
  slave::Slave::applyOperation(message);
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
