#ifndef __TESTING_UTILS_HPP__
#define __TESTING_UTILS_HPP__

#include <gmock/gmock.h>

#include <string>

#include <mesos/executor.hpp>
#include <mesos/scheduler.hpp>

#include <process/process.hpp>

#include "messaging/messages.hpp"


namespace mesos { namespace internal { namespace test {

/**
 * The location where Mesos is installed, used by tests to locate various
 * frameworks and binaries. For now it points to the src directory, until
 * we clean up our directory structure a little. Initialized in main.cpp.
 */
extern std::string mesosHome;


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
  TEST(testCase, testName) { \
    enterTestDirectory(#testCase, #testName); \
    runTestBody_##testCase##_##testName(); \
  } \
  void runTestBody_##testCase##_##testName() /* User code block follows */


/**
 * Macros to get a "default" dummy ExecutorInfo object for testing or
 * create one out of an ExecutorID and string.
 */
#define DEFAULT_EXECUTOR_INFO                                           \
  ({ ExecutorInfo executor;                                             \
    executor.mutable_executor_id()->set_value("default");               \
    executor.set_uri("noexecutor");                                     \
    executor; })


#define CREATE_EXECUTOR_INFO(executorId, uri)                           \
  ({ ExecutorInfo executor;                                             \
    executor.mutable_executor_id()->MergeFrom(executorId);              \
    executor.set_uri(uri);                                              \
    executor; })


#define DEFAULT_EXECUTOR_ID						\
      DEFAULT_EXECUTOR_INFO.executor_id()


/**
 * Definition of a mock Scheduler to be used in tests with gmock.
 */
class MockScheduler : public Scheduler
{
public:
  MOCK_METHOD1(getFrameworkName, std::string(SchedulerDriver*));
  MOCK_METHOD1(getExecutorInfo, ExecutorInfo(SchedulerDriver*));
  MOCK_METHOD2(registered, void(SchedulerDriver*, const FrameworkID&));
  MOCK_METHOD3(resourceOffer, void(SchedulerDriver*, const OfferID&,
                                   const std::vector<SlaveOffer>&));
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
  MOCK_METHOD2(init, void(ExecutorDriver*, const ExecutorArgs&));
  MOCK_METHOD2(launchTask, void(ExecutorDriver*, const TaskDescription&));
  MOCK_METHOD2(killTask, void(ExecutorDriver*, const TaskID&));
  MOCK_METHOD2(frameworkMessage, void(ExecutorDriver*, const std::string&));
  MOCK_METHOD1(shutdown, void(ExecutorDriver*));
  MOCK_METHOD3(error, void(ExecutorDriver*, int, const std::string&));
};


/**
 * Definition of a mock Filter so that messages can act as triggers.
 */
class MockFilter : public process::Filter
{
public:
  MOCK_METHOD1(filter, bool(process::Message *));
};


/**
 * A message can be matched against in conjunction with the MockFilter
 * (see above) to perform specific actions based for messages.
 */
MATCHER_P3(MsgMatcher, name, from, to, "")
{
  return (testing::Matcher<std::string>(name).Matches(arg->name) &&
          testing::Matcher<process::UPID>(from).Matches(arg->from) &&
          testing::Matcher<process::UPID>(to).Matches(arg->to));
}


/**
 * This macro provides some syntactic sugar for matching messages
 * using the message matcher (see above) as well as the MockFilter
 * (see above).
 */
#define EXPECT_MSG(filter, name, from, to)                \
  EXPECT_CALL(filter, filter(MsgMatcher(name, from, to)))


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
      if (trigger.value)                                                \
        break;                                                          \
      usleep(10);                                                       \
      if (sleeps++ >= 200000) {                                         \
        FAIL() << "Waited too long for trigger!";                       \
        abort; /* TODO(benh): Don't abort here ... */                   \
        break;                                                          \
      }                                                                 \
    } while (true);                                                     \
  } while (false)


}}} // namespace mesos::internal::test


#endif /* __TESTING_UTILS_HPP__ */
