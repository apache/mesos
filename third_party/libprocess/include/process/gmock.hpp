#ifndef __PROCESS_GMOCK_HPP__
#define __PROCESS_GMOCK_HPP__

#include <gmock/gmock.h>

#include <tr1/tuple>

#include <process/dispatch.hpp>
#include <process/event.hpp>
#include <process/filter.hpp>
#include <process/pid.hpp>

#include <stout/exit.hpp>


#define EXPECT_MESSAGE(name, from, to)                                  \
  EXPECT_CALL(*process::FilterTestEventListener::instance()->install(), \
              filter(testing::A<const process::MessageEvent&>()))       \
    .With(process::MessageMatcher(name, from, to))


#define EXPECT_DISPATCH(pid, method)                                    \
  EXPECT_CALL(*process::FilterTestEventListener::instance()->install(), \
              filter(testing::A<const process::DispatchEvent&>()))      \
    .With(process::DispatchMatcher(pid, method))


namespace process {

// Used by EXPECT_MESSAGE for matching a MessageEvent.
MATCHER_P3(MessageMatcher, name, from, to, "")
{
  const MessageEvent& event = ::std::tr1::get<0>(arg);
  return (testing::Matcher<std::string>(name).Matches(event.message->name) &&
          testing::Matcher<UPID>(from).Matches(event.message->from) &&
          testing::Matcher<UPID>(to).Matches(event.message->to));
}


// Used by EXPECT_DISPATCH for matching a DispatchEvent.
MATCHER_P2(DispatchMatcher, pid, method, "")
{
  const DispatchEvent& event = ::std::tr1::get<0>(arg);
  return (testing::Matcher<UPID>(pid).Matches(event.pid) &&
          testing::Matcher<std::string>(internal::canonicalize(method))
          .Matches(event.method));
}


// A definition of a libprocess filter to enable waiting for events
// (such as messages or dispatches) via WAIT_UNTIL in tests (i.e.,
// using triggers). This is not meant to be used directly by tests;
// tests should use macros like EXPECT_MESSAGE.
class TestsFilter : public Filter
{
public:
  TestsFilter()
  {
    EXPECT_CALL(*this, filter(testing::A<const MessageEvent&>()))
      .WillRepeatedly(testing::Return(false));
    EXPECT_CALL(*this, filter(testing::A<const DispatchEvent&>()))
      .WillRepeatedly(testing::Return(false));
    EXPECT_CALL(*this, filter(testing::A<const HttpEvent&>()))
      .WillRepeatedly(testing::Return(false));
    EXPECT_CALL(*this, filter(testing::A<const ExitedEvent&>()))
      .WillRepeatedly(testing::Return(false));
  }

  MOCK_METHOD1(filter, bool(const MessageEvent&));
  MOCK_METHOD1(filter, bool(const DispatchEvent&));
  MOCK_METHOD1(filter, bool(const HttpEvent&));
  MOCK_METHOD1(filter, bool(const ExitedEvent&));
};


class FilterTestEventListener : public ::testing::EmptyTestEventListener
{
public:
  // Returns the singleton instance of the listener.
  static FilterTestEventListener* instance()
  {
    static FilterTestEventListener* listener = new FilterTestEventListener();
    return listener;
  }

  // Installs and returns the filter, creating it if necessary.
  TestsFilter* install()
  {
    if (!started) {
      EXIT(1)
        << "To use EXPECT_MESSAGE you need to do the following before you "
        << "invoke RUN_ALL_TESTS():\n\n"
        << "\t::testing::TestEventListeners& listeners =\n"
        << "\t  ::testing::UnitTest::GetInstance()->listeners();\n"
        << "\tlisteners.Append(process::FilterTestEventListener::instance());";
    }

    if (filter != NULL) {
      return filter;
    }

    filter = new TestsFilter();

    // Set the filter in libprocess.
    process::filter(filter);

    return filter;
  }

  virtual void OnTestProgramStart(const ::testing::UnitTest&)
  {
    started = true;
  }

  virtual void OnTestEnd(const ::testing::TestInfo&)
  {
    if (filter != NULL) {
      // Remove the filter in libprocess _before_ deleting.
      process::filter(NULL);
      delete filter;
      filter = NULL;
    }
  }

private:
  FilterTestEventListener() : filter(NULL), started(false) {}

  TestsFilter* filter;

  // Indicates if we got the OnTestProgramStart callback in order to
  // detect if we have been properly added as a listener.
  bool started;
};

} // namespace process {

#endif // __PROCESS_GMOCK_HPP__
