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

#include <gmock/gmock.h>

#include <mesos/authentication/secret_generator.hpp>

#include <mesos/slave/qos_controller.hpp>
#include <mesos/slave/resource_estimator.hpp>

#include <process/future.hpp>
#include <process/pid.hpp>

#include <stout/option.hpp>

#include "slave/slave.hpp"
#include "slave/status_update_manager.hpp"

#include "tests/mock_slave.hpp"

using mesos::master::detector::MasterDetector;

using mesos::slave::ContainerTermination;

using std::list;

using process::Future;
using process::UPID;

using testing::_;
using testing::Invoke;

namespace mesos {
namespace internal {
namespace tests {

MockGarbageCollector::MockGarbageCollector()
{
  // NOTE: We use 'EXPECT_CALL' and 'WillRepeatedly' here instead of
  // 'ON_CALL' and 'WillByDefault'. See 'TestContainerizer::SetUp()'
  // for more details.
  EXPECT_CALL(*this, schedule(_, _))
    .WillRepeatedly(Return(Nothing()));

  EXPECT_CALL(*this, unschedule(_))
    .WillRepeatedly(Return(true));

  EXPECT_CALL(*this, prune(_))
    .WillRepeatedly(Return());
}


MockGarbageCollector::~MockGarbageCollector() {}


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
    const slave::Flags& flags,
    MasterDetector* detector,
    slave::Containerizer* containerizer,
    const Option<mesos::slave::QoSController*>& _qosController,
    const Option<mesos::Authorizer*>& authorizer,
    const Option<mesos::SecretGenerator*>& _mockSecretGenerator)
  : slave::Slave(
        process::ID::generate("slave"),
        flags,
        detector,
        containerizer,
        &files,
        &gc,
        statusUpdateManager = new slave::StatusUpdateManager(flags),
        &resourceEstimator,
        _qosController.isSome() ? _qosController.get() : &qosController,
        authorizer),
    files(slave::READONLY_HTTP_AUTHENTICATION_REALM),
    mockSecretGenerator(_mockSecretGenerator)
{
  // Set up default behaviors, calling the original methods.
  EXPECT_CALL(*this, runTask(_, _, _, _, _))
    .WillRepeatedly(Invoke(this, &MockSlave::unmocked_runTask));
  EXPECT_CALL(*this, _run(_, _, _, _, _))
    .WillRepeatedly(Invoke(this, &MockSlave::unmocked__run));
  EXPECT_CALL(*this, runTaskGroup(_, _, _, _))
    .WillRepeatedly(Invoke(this, &MockSlave::unmocked_runTaskGroup));
  EXPECT_CALL(*this, killTask(_, _))
    .WillRepeatedly(Invoke(this, &MockSlave::unmocked_killTask));
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
}


MockSlave::~MockSlave()
{
  delete statusUpdateManager;
}


void MockSlave::initialize()
{
  Slave::initialize();

  if (mockSecretGenerator.isSome()) {
    delete secretGenerator;
    secretGenerator = mockSecretGenerator.get();
    mockSecretGenerator = None();
  }
}


void MockSlave::unmocked_runTask(
    const UPID& from,
    const FrameworkInfo& frameworkInfo,
    const FrameworkID& frameworkId,
    const UPID& pid,
    const TaskInfo& task)
{
  slave::Slave::runTask(from, frameworkInfo, frameworkInfo.id(), pid, task);
}


void MockSlave::unmocked__run(
    const Future<bool>& future,
    const FrameworkInfo& frameworkInfo,
    const ExecutorInfo& executorInfo,
    const Option<TaskInfo>& taskInfo,
    const Option<TaskGroupInfo>& taskGroup)
{
  slave::Slave::_run(
      future, frameworkInfo, executorInfo, taskInfo, taskGroup);
}


void MockSlave::unmocked_runTaskGroup(
    const UPID& from,
    const FrameworkInfo& frameworkInfo,
    const ExecutorInfo& executorInfo,
    const TaskGroupInfo& taskGroup)
{
  slave::Slave::runTaskGroup(from, frameworkInfo, executorInfo, taskGroup);
}


void MockSlave::unmocked_killTask(
    const UPID& from,
    const KillTaskMessage& killTaskMessage)
{
  slave::Slave::killTask(from, killTaskMessage);
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

} // namespace tests {
} // namespace internal {
} // namespace mesos {
