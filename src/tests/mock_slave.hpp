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

#ifndef __TESTS_MOCKSLAVE_HPP__
#define __TESTS_MOCKSLAVE_HPP__

#include <list>
#include <string>

#include <gmock/gmock.h>

#include <mesos/authentication/secret_generator.hpp>

#include <mesos/master/detector.hpp>

#include <mesos/slave/qos_controller.hpp>
#include <mesos/slave/resource_estimator.hpp>

#include <process/future.hpp>
#include <process/pid.hpp>

#include <stout/duration.hpp>
#include <stout/none.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

#include "messages/messages.hpp"

#include "slave/slave.hpp"

using ::testing::_;
using ::testing::DoDefault;
using ::testing::Return;

namespace mesos {
namespace internal {
namespace tests {

class MockGarbageCollector : public slave::GarbageCollector
{
public:
  MockGarbageCollector();
  virtual ~MockGarbageCollector();

  MOCK_METHOD2(
      schedule,
      process::Future<Nothing>(const Duration& d, const std::string& path));
  MOCK_METHOD1(
      unschedule,
      process::Future<bool>(const std::string& path));
  MOCK_METHOD1(
      prune,
      void(const Duration& d));
};


class MockResourceEstimator : public mesos::slave::ResourceEstimator
{
public:
  MockResourceEstimator();
  virtual ~MockResourceEstimator();

  MOCK_METHOD1(
      initialize,
      Try<Nothing>(const lambda::function<process::Future<ResourceUsage>()>&));

  MOCK_METHOD0(
      oversubscribable,
      process::Future<Resources>());
};


// The MockQoSController is a stub which lets tests fill the
// correction queue for a slave.
class MockQoSController : public mesos::slave::QoSController
{
public:
  MockQoSController();
  virtual ~MockQoSController();

  MOCK_METHOD1(
      initialize,
      Try<Nothing>(const lambda::function<process::Future<ResourceUsage>()>&));

  MOCK_METHOD0(
      corrections, process::Future<std::list<mesos::slave::QoSCorrection>>());
};


// Definition of a mock Slave to be used in tests with gmock, covering
// potential races between runTask and killTask.
class MockSlave : public slave::Slave
{
public:
  MockSlave(
      const slave::Flags& flags,
      mesos::master::detector::MasterDetector* detector,
      slave::Containerizer* containerizer,
      const Option<mesos::slave::QoSController*>& qosController = None(),
      const Option<mesos::Authorizer*>& authorizer = None(),
      const Option<mesos::SecretGenerator*>& mockSecretGenerator = None());

  virtual ~MockSlave();

  void initialize();

  MOCK_METHOD5(runTask, void(
      const process::UPID& from,
      const FrameworkInfo& frameworkInfo,
      const FrameworkID& frameworkId,
      const process::UPID& pid,
      const TaskInfo& task));

  void unmocked_runTask(
      const process::UPID& from,
      const FrameworkInfo& frameworkInfo,
      const FrameworkID& frameworkId,
      const process::UPID& pid,
      const TaskInfo& task);

  MOCK_METHOD5(_run, void(
      const process::Future<bool>& future,
      const FrameworkInfo& frameworkInfo,
      const ExecutorInfo& executorInfo,
      const Option<TaskInfo>& task,
      const Option<TaskGroupInfo>& taskGroup));

  void unmocked__run(
      const process::Future<bool>& future,
      const FrameworkInfo& frameworkInfo,
      const ExecutorInfo& executorInfo,
      const Option<TaskInfo>& task,
      const Option<TaskGroupInfo>& taskGroup);

  MOCK_METHOD4(runTaskGroup, void(
      const process::UPID& from,
      const FrameworkInfo& frameworkInfo,
      const ExecutorInfo& executorInfo,
      const TaskGroupInfo& taskGroup));

  void unmocked_runTaskGroup(
      const process::UPID& from,
      const FrameworkInfo& frameworkInfo,
      const ExecutorInfo& executorInfo,
      const TaskGroupInfo& taskGroup);

  MOCK_METHOD2(killTask, void(
      const process::UPID& from,
      const KillTaskMessage& killTaskMessage));

  void unmocked_killTask(
      const process::UPID& from,
      const KillTaskMessage& killTaskMessage);

  MOCK_METHOD1(removeFramework, void(
      slave::Framework* framework));

  void unmocked_removeFramework(
      slave::Framework* framework);

  MOCK_METHOD1(__recover, void(
      const process::Future<Nothing>& future));

  void unmocked___recover(
      const process::Future<Nothing>& future);

  MOCK_METHOD0(qosCorrections, void());

  void unmocked_qosCorrections();

  MOCK_METHOD1(_qosCorrections, void(
      const process::Future<std::list<
          mesos::slave::QoSCorrection>>& correction));

  MOCK_METHOD0(usage, process::Future<ResourceUsage>());

  process::Future<ResourceUsage> unmocked_usage();

  MOCK_METHOD3(executorTerminated, void(
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      const process::Future<Option<
          mesos::slave::ContainerTermination>>& termination));

  void unmocked_executorTerminated(
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      const process::Future<Option<
          mesos::slave::ContainerTermination>>& termination);

  MOCK_METHOD3(shutdownExecutor, void(
      const process::UPID& from,
      const FrameworkID& frameworkId,
      const ExecutorID& executorId));

  void unmocked_shutdownExecutor(
      const process::UPID& from,
      const FrameworkID& frameworkId,
      const ExecutorID& executorId);

private:
  Files files;
  MockGarbageCollector gc;
  MockResourceEstimator resourceEstimator;
  MockQoSController qosController;
  slave::StatusUpdateManager* statusUpdateManager;

  // Set to the base class `secretGenerator` in `initialize()`. After
  // `initialize()` has executed, this will be `None()`.
  Option<mesos::SecretGenerator*> mockSecretGenerator;
};

} // namespace tests {
} // namespace internal {
} // namespace mesos {

#endif // __TESTS_MOCKSLAVE_HPP__
