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

#ifndef __TESTS_MASTER_MOCK_API_SUBSCRIBER__
#define __TESTS_MASTER_MOCK_API_SUBSCRIBER__

#include <memory>

#include <gmock/gmock.h>

#include <process/future.hpp>
#include <process/pid.hpp>

#include <mesos/v1/master/master.hpp>
#include <mesos/http.hpp>

#include "master/master.hpp"

namespace mesos {
namespace internal {
namespace tests {
namespace v1 {

class MockMasterAPISubscriberProcess;

// This class performs subscribing to master's V1 API events
// and provides mock methods for setting expectations for these events
// similarly to MockHTTPScheduler.
class MockMasterAPISubscriber
{
public:
  // Mock methods which are called by this class when an event is received.
  MOCK_METHOD1(subscribed, void(const ::mesos::v1::master::Event::Subscribed&));

  MOCK_METHOD1(taskAdded, void(const ::mesos::v1::master::Event::TaskAdded&));

  MOCK_METHOD1(taskUpdated,
               void(const ::mesos::v1::master::Event::TaskUpdated&));

  MOCK_METHOD1(agentAdded,
               void(const ::mesos::v1::master::Event::AgentAdded&));

  MOCK_METHOD1(agentRemoved,
               void(const ::mesos::v1::master::Event::AgentRemoved&));

  MOCK_METHOD1(frameworkAdded,
               void(const ::mesos::v1::master::Event::FrameworkAdded&));

  MOCK_METHOD1(frameworkUpdated,
               void(const ::mesos::v1::master::Event::FrameworkUpdated&));

  MOCK_METHOD1(frameworkRemoved,
               void(const ::mesos::v1::master::Event::FrameworkRemoved&));

  MOCK_METHOD0(heartbeat, void());

  MockMasterAPISubscriber();
  virtual ~MockMasterAPISubscriber();

  // Subscribes to the master's V1 API event stream.
  // The returned Future becomes ready after the SUBSCRIBE call returns a
  // 200 OK response.
  //
  // NOTE: This method should be called at most once per the lifetime of the
  // mock.
  //
  // NOTE: All expectations on the mock methods should be set before calling
  // this method.
  process::Future<Nothing> subscribe(
    const process::PID<mesos::internal::master::Master>& masterPid,
    ContentType contentType = ContentType::PROTOBUF);

private:
  friend class MockMasterAPISubscriberProcess;
  void handleEvent(const ::mesos::v1::master::Event& event);

  bool subscribeCalled;
  process::PID<MockMasterAPISubscriberProcess> pid;
};


} // namespace v1 {
} // namespace tests {
} // namespace internal {
} // namespace mesos {

#endif //  __TESTS_MASTER_MOCK_API_SUBSCRIBER__
