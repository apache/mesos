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

#ifndef __TEST_ISOLATOR_HPP__
#define __TEST_ISOLATOR_HPP__

#include <gmock/gmock.h>

#include "slave/containerizer/mesos/isolator.hpp"

namespace mesos {
namespace internal {
namespace tests {

// Provides a mock Isolator that by default expects calls to
// Isolator::prepare, Isolator::isolate, Isolator::watch, and
// Isolator::cleanup and simply returns "nothing" as appropriate for
// each call. This behavior can be overridden by adding EXPECT_CALL as
// necessary. For example, if you don't expect any calls to
// Isolator::cleanup you can do:
//
//   MockIsolator isolator;
//   EXPECT_CALL(isolator, prepare(_, _))
//    .Times(0);
//
// Or if you want to override that only a single invocation should
// occur you can do:
//
//   MockIsolator isolator;
//   EXPECT_CALL(isolator, prepare(_, _))
//    .WillOnce(Return(ContainerLaunchInfo(...)));
//
// But note that YOU MUST use exactly `prepare(_, _)` otherwise gmock
// will not properly match this new expectation with the default
// expectation created by MockIsolator.
//
// In the event you want to override a single invocation but let all
// subsequent invocations return the "default" you can do:
//
//   MockIsolator isolator;
//   EXPECT_CALL(isolator, prepare(_, _))
//    .WillOnce(Return(ContainerLaunchInfo(...)))
//    .RetiresOnSaturation();
//
// Another example, if you want to override what gets returned for a
// every invocation you can do:
//
//   MockIsolator isolator;
//   EXPECT_CALL(isolator, prepare(_, _))
//    .WillRepeatedly(Return(Failure(...)));
//
// Again, YOU MUST use exactly `prepare(_, _)` to override the default
// expectation.
class MockIsolator : public mesos::slave::Isolator
{
public:
  MockIsolator()
  {
    EXPECT_CALL(*this, prepare(_, _))
      .WillRepeatedly(Return(None()));

    EXPECT_CALL(*this, isolate(_, _))
      .WillRepeatedly(Return(Nothing()));

    EXPECT_CALL(*this, watch(_))
      .WillRepeatedly(
          Return(process::Future<mesos::slave::ContainerLimitation>()));

    EXPECT_CALL(*this, cleanup(_))
      .WillRepeatedly(Return(Nothing()));
  }

  MOCK_METHOD2(
      recover,
      process::Future<Nothing>(
          const std::vector<mesos::slave::ContainerState>&,
          const hashset<ContainerID>&));

  MOCK_METHOD2(
      prepare,
      process::Future<Option<mesos::slave::ContainerLaunchInfo>>(
          const ContainerID&,
          const mesos::slave::ContainerConfig&));

  MOCK_METHOD2(
      isolate,
      process::Future<Nothing>(const ContainerID&, pid_t));

  MOCK_METHOD1(
      watch,
      process::Future<mesos::slave::ContainerLimitation>(const ContainerID&));

  MOCK_METHOD3(
      update,
      process::Future<Nothing>(
          const ContainerID&,
          const Resources&,
          const google::protobuf::Map<std::string, Value::Scalar>&));

  MOCK_METHOD1(
      usage,
      process::Future<ResourceStatistics>(const ContainerID&));

  MOCK_METHOD1(
      cleanup,
      process::Future<Nothing>(const ContainerID&));
};

} // namespace tests {
} // namespace internal {
} // namespace mesos {

#endif // __TEST_ISOLATOR_HPP__
