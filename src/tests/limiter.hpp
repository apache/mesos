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

#ifndef __TEST_LIMITER_HPP__
#define __TEST_LIMITER_HPP__

#include <gmock/gmock.h>

#include <process/limiter.hpp>

#include <stout/duration.hpp>

namespace mesos {
namespace internal {
namespace tests {

class MockRateLimiter : public process::RateLimiter
{
public:
  // We initialize the rate limiter to 1 per second because it has to
  // be non-zero, but this value has no effect since this is a mock.
  MockRateLimiter() : process::RateLimiter(1, Seconds(1)) {}

  MOCK_CONST_METHOD0(acquire, process::Future<Nothing>());
};

} // namespace tests {
} // namespace internal {
} // namespace mesos {

#endif // __TEST_LIMITER_HPP__
