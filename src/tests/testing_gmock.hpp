#ifndef __TESTING_GMOCK_HPP__
#define __TESTING_GMOCK_HPP__

#include <string>

#include <gmock/gmock.h>


namespace mesos { namespace internal { namespace test {

/**
 * Definition of a mock Scheduler to be used in tests with gmock.
 */
class MockScheduler : public Scheduler
{
public:
  MOCK_METHOD1(getFrameworkName, std::string(SchedulerDriver*));
  MOCK_METHOD1(getExecutorInfo, ExecutorInfo(SchedulerDriver*));
  MOCK_METHOD2(registered, void(SchedulerDriver*, FrameworkID));
  MOCK_METHOD3(resourceOffer, void(SchedulerDriver*, OfferID,
                                   const std::vector<SlaveOffer>&));
  MOCK_METHOD2(offerRescinded, void(SchedulerDriver*, OfferID));
  MOCK_METHOD2(statusUpdate, void(SchedulerDriver*, const TaskStatus&));
  MOCK_METHOD2(frameworkMessage, void(SchedulerDriver*,
                                      const FrameworkMessage&));
  MOCK_METHOD2(slaveLost, void(SchedulerDriver*, SlaveID));
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
  MOCK_METHOD2(killTask, void(ExecutorDriver*, TaskID));
  MOCK_METHOD2(frameworkMessage, void(ExecutorDriver*, const FrameworkMessage&));
  MOCK_METHOD1(shutdown, void(ExecutorDriver*));
  MOCK_METHOD3(error, void(ExecutorDriver*, int, const std::string&));
};


/**
 * Definition of a mock Filter so that messages can act as triggers.
 */
class MockFilter : public MessageFilter
{
 public:
  MOCK_METHOD1(filter, bool(struct msg *));
};


/**
 * A message can be matched against in conjunction with the MockFilter
 * (see above) to perform specific actions based for messages.
 */
MATCHER_P3(MsgMatcher, id, from, to, "")
{
  return (testing::Matcher<MSGID>(id).Matches(arg->id) &&
          testing::Matcher<PID>(from).Matches(arg->from) &&
          testing::Matcher<PID>(to).Matches(arg->to));
}


/**
 * This macro provides some syntactic sugar for matching messages
 * using the message matcher (see above) as well as the MockFilter
 * (see above).
 */
#define EXPECT_MSG(filter, id, from, to)                \
  EXPECT_CALL(filter, filter(MsgMatcher(id, from, to)))


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
        ADD_FAILURE() << "Waited too long for trigger!";                \
        break;                                                          \
      }                                                                 \
    } while (true);                                                     \
  } while (false)


}}} // namespace mesos { namespace internal { namespace test {


#endif /* __TESTING_GMOCK_HPP__ */
