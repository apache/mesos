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

#ifndef __TESTS_FILTER_HPP__
#define __TESTS_FILTER_HPP__

#include <gmock/gmock.h>

#include <process/event.hpp>
#include <process/filter.hpp>


// This macro provides a mechanism for matching libprocess
// messages. TODO(benh): Also add EXPECT_DISPATCH, EXPECT_HTTP, etc.
#define EXPECT_MESSAGE(name, from, to)                                  \
  EXPECT_CALL(*mesos::internal::tests::filter(),                        \
              filter(testing::A<const process::MessageEvent&>()))       \
    .With(mesos::internal::tests::MessageMatcher(name, from, to))


namespace mesos {
namespace internal {
namespace tests {

// A gtest matcher used by EXPECT_MESSAGE for matching a libprocess
// MessageEvent.
MATCHER_P3(MessageMatcher, name, from, to, "")
{
  const process::MessageEvent& event = ::std::tr1::get<0>(arg);
  return (testing::Matcher<std::string>(name).Matches(event.message->name) &&
          testing::Matcher<process::UPID>(from).Matches(event.message->from) &&
          testing::Matcher<process::UPID>(to).Matches(event.message->to));
}


// A definition of a libprocess filter to enable waiting for events
// (such as messages or dispatches) via WAIT_UNTIL in tests (i.e.,
// using triggers). This is not meant to be used directly by tests;
// tests should use macros like EXPECT_MESSAGE.
class TestsFilter : public process::Filter
{
public:
  // A "singleton" instance of Filter that gets used by a single
  // test. The 'FilterTestEventListener' deletes this instance after
  // each test to make sure no filter is set for subsequent tests. The
  // 'filter' routine constructs a new "singleton" if no instance yet
  // exists.
  static TestsFilter* instance;

  TestsFilter()
  {
    EXPECT_CALL(*this, filter(testing::A<const process::MessageEvent&>()))
      .WillRepeatedly(testing::Return(false));
    EXPECT_CALL(*this, filter(testing::A<const process::DispatchEvent&>()))
      .WillRepeatedly(testing::Return(false));
    EXPECT_CALL(*this, filter(testing::A<const process::HttpEvent&>()))
      .WillRepeatedly(testing::Return(false));
    EXPECT_CALL(*this, filter(testing::A<const process::ExitedEvent&>()))
      .WillRepeatedly(testing::Return(false));

    process::filter(this);
  }

  virtual ~TestsFilter()
  {
    process::filter(NULL);
  }

  MOCK_METHOD1(filter, bool(const process::MessageEvent&));
  MOCK_METHOD1(filter, bool(const process::DispatchEvent&));
  MOCK_METHOD1(filter, bool(const process::HttpEvent&));
  MOCK_METHOD1(filter, bool(const process::ExitedEvent&));
};


inline TestsFilter* filter()
{
  if (TestsFilter::instance != NULL) {
    return TestsFilter::instance;
  }

  return TestsFilter::instance = new TestsFilter();
}


class FilterTestEventListener : public ::testing::EmptyTestEventListener
{
public:
  virtual void OnTestEnd(const ::testing::TestInfo&)
  {
    if (TestsFilter::instance != NULL) {
      delete TestsFilter::instance;
      TestsFilter::instance = NULL;
    }
  }
};

} // namespace tests {
} // namespace internal {
} // namespace mesos {

#endif // __TESTS_FILTER_HPP__
