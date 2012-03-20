/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __TESTING_UTILS_HPP__
#define __TESTING_UTILS_HPP__

#include <gmock/gmock.h>

#include <map>
#include <string>

#include <master/allocator.hpp>
#include <master/master.hpp>
#include <mesos/executor.hpp>
#include <mesos/scheduler.hpp>

#include <process/process.hpp>

#include "common/utils.hpp"
#include "common/type_utils.hpp"

#include "messages/messages.hpp"

#include "slave/isolation_module.hpp"
#include "slave/slave.hpp"


namespace mesos {
namespace internal {
namespace test {

/**
 * The location of the Mesos source directory.  Used by tests to locate
 * various frameworks and binaries.  Initialized in main.cpp.
 */
extern std::string mesosSourceDirectory;


/**
 * The location of the Mesos build directory. Used by tests to locate
 * frameworks and binaries.  Initialized in main.cpp.
 */
extern std::string mesosBuildDirectory;


/**
 * Create and clean up the work directory for a given test, and cd into it,
 * given the test's test case name and test name.
 * Test directories are placed in <mesosHome>/test_output/<testCase>/<testName>.
 */
void enterTestDirectory(const char* testCase, const char* testName);


/**
 * Macro for running a test in a work directory (using enterTestDirectory).
 * Used in a similar way to gtest's TEST macro (by adding a body in braces).
 */
#define TEST_WITH_WORKDIR(testCase, testName) \
  void runTestBody_##testCase##_##testName(); \
  TEST(testCase, testName) {                  \
    enterTestDirectory(#testCase, #testName); \
    runTestBody_##testCase##_##testName(); \
  } \
  void runTestBody_##testCase##_##testName() /* User code block follows */


/**
 * Macros to get a "default" dummy ExecutorInfo object for testing or
 * create one out of an ExecutorID and string.
 */
#define DEFAULT_EXECUTOR_INFO                                           \
      ({ ExecutorInfo executor;                                         \
        executor.mutable_executor_id()->set_value("default");           \
        executor.mutable_command()->set_value("exit 1");                \
        executor; })


#define CREATE_EXECUTOR_INFO(executorId, uri)                           \
      ({ ExecutorInfo executor;                                         \
        executor.mutable_executor_id()->MergeFrom(executorId);          \
        executor.mutable_command()->set_value("exit 1");                \
        executor; })


#define DEFAULT_EXECUTOR_ID						\
      DEFAULT_EXECUTOR_INFO.executor_id()


/**
 * Definition of a mock Scheduler to be used in tests with gmock.
 */
class MockScheduler : public Scheduler
{
public:
  MOCK_METHOD2(registered, void(SchedulerDriver*, const FrameworkID&));
  MOCK_METHOD2(resourceOffers, void(SchedulerDriver*,
                                    const std::vector<Offer>&));
  MOCK_METHOD2(offerRescinded, void(SchedulerDriver*, const OfferID&));
  MOCK_METHOD2(statusUpdate, void(SchedulerDriver*, const TaskStatus&));
  MOCK_METHOD4(frameworkMessage, void(SchedulerDriver*,
                                      const SlaveID&,
                                      const ExecutorID&,
                                      const std::string&));
  MOCK_METHOD2(slaveLost, void(SchedulerDriver*, const SlaveID&));
  MOCK_METHOD3(error, void(SchedulerDriver*, int, const std::string&));
};


/**
 * Definition of a mock Executor to be used in tests with gmock.
 */
class MockExecutor : public Executor
{
public:
  MOCK_METHOD6(registered, void(ExecutorDriver*,
                                const ExecutorInfo&,
                                const FrameworkID&,
                                const FrameworkInfo&,
                                const SlaveID&,
                                const SlaveInfo&));
  MOCK_METHOD2(launchTask, void(ExecutorDriver*, const TaskInfo&));
  MOCK_METHOD2(killTask, void(ExecutorDriver*, const TaskID&));
  MOCK_METHOD2(frameworkMessage, void(ExecutorDriver*, const std::string&));
  MOCK_METHOD1(shutdown, void(ExecutorDriver*));
  MOCK_METHOD3(error, void(ExecutorDriver*, int, const std::string&));
};


class MockAllocator : public master::Allocator
{
public:
  MOCK_METHOD1(initialize, void(master::Master*));
  MOCK_METHOD1(frameworkAdded, void(master::Framework*));
  MOCK_METHOD1(frameworkRemoved, void(master::Framework*));
  MOCK_METHOD1(slaveAdded, void(master::Slave*));
  MOCK_METHOD1(slaveRemoved, void(master::Slave*));
  MOCK_METHOD2(resourcesRequested, void(const FrameworkID&,
                                        const std::vector<Request>&));
  MOCK_METHOD3(resourcesUnused, void(const FrameworkID&,
                                     const SlaveID&,
                                     const Resources&));
  MOCK_METHOD3(resourcesRecovered, void(const FrameworkID&,
                                        const SlaveID&,
                                        const Resources&));
  MOCK_METHOD1(offersRevived, void(master::Framework*));
  MOCK_METHOD0(timerTick, void());
};


/**
 * Definition of a mock Filter so that messages can act as triggers.
 */
class MockFilter : public process::Filter
{
public:
  MockFilter()
  {
    EXPECT_CALL(*this, filter(testing::A<const process::MessageEvent&>()))
      .WillRepeatedly(testing::Return(false));
    EXPECT_CALL(*this, filter(testing::A<const process::DispatchEvent&>()))
      .WillRepeatedly(testing::Return(false));
    EXPECT_CALL(*this, filter(testing::A<const process::HttpEvent&>()))
      .WillRepeatedly(testing::Return(false));
    EXPECT_CALL(*this, filter(testing::A<const process::ExitedEvent&>()))
      .WillRepeatedly(testing::Return(false));
  }

  MOCK_METHOD1(filter, bool(const process::MessageEvent&));
  MOCK_METHOD1(filter, bool(const process::DispatchEvent&));
  MOCK_METHOD1(filter, bool(const process::HttpEvent&));
  MOCK_METHOD1(filter, bool(const process::ExitedEvent&));
};


/**
 * A message can be matched against in conjunction with the MockFilter
 * (see above) to perform specific actions based for messages.
 */
MATCHER_P3(MsgMatcher, name, from, to, "")
{
  const process::MessageEvent& event = ::std::tr1::get<0>(arg);
  return (testing::Matcher<std::string>(name).Matches(event.message->name) &&
          testing::Matcher<process::UPID>(from).Matches(event.message->from) &&
          testing::Matcher<process::UPID>(to).Matches(event.message->to));
}


/**
 * This macro provides some syntactic sugar for matching messages
 * using the message matcher (see above) as well as the MockFilter
 * (see above). We should also add EXPECT_DISPATCH, EXPECT_HTTP, etc.
 */
#define EXPECT_MESSAGE(mockFilter, name, from, to)              \
  EXPECT_CALL(mockFilter, filter(testing::A<const process::MessageEvent&>())) \
    .With(MsgMatcher(name, from, to))


ACTION_TEMPLATE(SaveArgField,
                HAS_1_TEMPLATE_PARAMS(int, k),
                AND_2_VALUE_PARAMS(field, pointer))
{
  *pointer = *(::std::tr1::get<k>(args).*field);
}


/**
 * A trigger is an object that can be used to effectively block a test
 * from proceeding until some event has occured. A trigger can get set
 * using a gmock action (see below) and you can wait for a trigger to
 * occur using the WAIT_UNTIL macro below.
 */
struct trigger
{
  trigger() : value(false) {}
  bool value;
};


/**
 * Definition of the Trigger action to be used with gmock.
 */
ACTION_P(Trigger, trigger) { trigger->value = true; }


/**
 * Definition of the SendStatusUpdate action to be used with gmock.
 */
ACTION_P(SendStatusUpdate, state)
{
  TaskStatus status;
  status.mutable_task_id()->MergeFrom(arg1.task_id());
  status.set_state(state);
  arg0->sendStatusUpdate(status);
}


/**
 * This macro can be used to wait until some trigger has
 * occured. Currently, a test will wait no longer than approxiamtely 2
 * seconds (10 us * 200000). At some point we may add a mechanism to
 * specify how long to try and wait.
 */
#define WAIT_UNTIL(trigger)                                             \
  do {                                                                  \
    int sleeps = 0;                                                     \
    do {                                                                \
      __sync_synchronize();                                             \
      if ((trigger).value)                                              \
        break;                                                          \
      usleep(10);                                                       \
      if (sleeps++ >= 200000) {                                         \
        FAIL() << "Waited too long for trigger!";                       \
        ::exit(-1); /* TODO(benh): Figure out how not to exit! */       \
        break;                                                          \
      }                                                                 \
    } while (true);                                                     \
  } while (false)


class TestingIsolationModule : public slave::IsolationModule
{
public:
  TestingIsolationModule(const std::map<ExecutorID, Executor*>& _executors)
    : executors(_executors)
  {
    EXPECT_CALL(*this, resourcesChanged(testing::_, testing::_, testing::_))
      .Times(testing::AnyNumber());
  }

  virtual ~TestingIsolationModule() {}

  virtual void initialize(const Configuration& conf,
                          bool local,
                          const process::PID<slave::Slave>& _slave)
  {
    slave = _slave;
  }

  virtual void launchExecutor(const FrameworkID& frameworkId,
                              const FrameworkInfo& frameworkInfo,
                              const ExecutorInfo& executorInfo,
                              const std::string& directory,
                              const Resources& resources)
  {
    if (executors.count(executorInfo.executor_id()) > 0) {
      Executor* executor = executors[executorInfo.executor_id()];
      MesosExecutorDriver* driver = new MesosExecutorDriver(executor);
      drivers[executorInfo.executor_id()] = driver;

      directories[executorInfo.executor_id()] = directory;

      utils::os::setenv("MESOS_LOCAL", "1");
      utils::os::setenv("MESOS_DIRECTORY", directory);
      utils::os::setenv("MESOS_SLAVE_PID", slave);
      utils::os::setenv("MESOS_FRAMEWORK_ID", frameworkId.value());
      utils::os::setenv("MESOS_EXECUTOR_ID", executorInfo.executor_id().value());

      driver->start();

      utils::os::unsetenv("MESOS_LOCAL");
      utils::os::unsetenv("MESOS_DIRECTORY");
      utils::os::unsetenv("MESOS_SLAVE_PID");
      utils::os::unsetenv("MESOS_FRAMEWORK_ID");
      utils::os::unsetenv("MESOS_EXECUTOR_ID");
    } else {
      FAIL() << "Cannot launch executor";
    }
  }

  virtual void killExecutor(const FrameworkID& frameworkId,
                            const ExecutorID& executorId)
  {
    if (drivers.count(executorId) > 0) {
      MesosExecutorDriver* driver = drivers[executorId];
      driver->stop();
      driver->join();
      delete driver;
      drivers.erase(executorId);
    } else {
      FAIL() << "Cannot kill executor";
    }
  }

  // Mocked so tests can check that the resources reflect all started tasks.
  MOCK_METHOD3(resourcesChanged, void(const FrameworkID&,
                                      const ExecutorID&,
                                      const Resources&));

  std::map<ExecutorID, std::string> directories;

private:
  std::map<ExecutorID, Executor*> executors;
  std::map<ExecutorID, MesosExecutorDriver*> drivers;
  process::PID<slave::Slave> slave;
};

} // namespace test {
} // namespace internal {
} // namespace mesos {

#endif // __TESTING_UTILS_HPP__
