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

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/owned.hpp>

#include <stout/lambda.hpp>

#include "common/future_tracker.hpp"

using std::string;
using std::vector;

using process::Future;
using process::Owned;
using process::Promise;

using testing::_;

namespace mesos {
namespace internal {
namespace tests {

class FutureTrackerTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    Try<PendingFutureTracker*> _tracker = PendingFutureTracker::create();
    ASSERT_FALSE(_tracker.isError());
    tracker.reset(_tracker.get());
  }

  Owned<PendingFutureTracker> tracker;
};


TEST_F(FutureTrackerTest, ListPending)
{
  Promise<bool> promise1;
  Promise<bool> promise2;

  Future<bool> future1 = promise1.future();
  Future<bool> future2 = promise2.future();

  const FutureMetadata data1{"f1", "test1", {}};
  const FutureMetadata data2{
    "f2", "test2", {{"arg1", "val1"}, {"arg2", "val2"}}};

  auto track1 =
    tracker->track(future1, data1.operation, data1.component, data1.args);

  auto track2 =
    tracker->track(future2, data2.operation, data2.component, data2.args);

  EXPECT_EQ(track1, future1);
  EXPECT_EQ(track2, future2);

  auto pending = tracker->pendingFutures();

  AWAIT_READY(pending);

  EXPECT_EQ(pending->size(), 2u);
  EXPECT_EQ(pending->at(0), data1);
  EXPECT_EQ(pending->at(1), data2);
}


TEST_F(FutureTrackerTest, ListReady)
{
  Promise<bool> promise1;
  Promise<bool> promise2;

  const FutureMetadata data1{"f1", "test1", {}};
  const FutureMetadata data2{"f2", "test2", {}};

  tracker->track(promise1.future(), data1.operation, data1.component);
  tracker->track(promise2.future(), data2.operation, data2.component);

  Future<Nothing> eraseFuture =
    FUTURE_DISPATCH(_, &PendingFutureTrackerProcess::eraseFuture);

  promise2.set(true);

  AWAIT_READY(eraseFuture);

  auto pending = tracker->pendingFutures();

  AWAIT_READY(pending);

  EXPECT_EQ(pending->size(), 1u);
  EXPECT_EQ(pending->front(), data1);
}


TEST_F(FutureTrackerTest, ListAfterCancellation)
{
  vector<lambda::function<void(Owned<Promise<bool>>&)>> cancellations = {
    [](Owned<Promise<bool>>& promise) {
      promise->discard();
    },
    [](Owned<Promise<bool>>& promise) {
      promise.reset();
    }
  };

  for (auto& cancel : cancellations) {
    Owned<Promise<bool>> promise(new Promise<bool>());

    tracker->track(promise->future(), "f", "test");

    Future<Nothing> eraseFuture =
      FUTURE_DISPATCH(_, &PendingFutureTrackerProcess::eraseFuture);

    cancel(promise);

    AWAIT_READY(eraseFuture);

    auto pending = tracker->pendingFutures();

    AWAIT_READY(pending);

    EXPECT_TRUE(pending->empty());
  }
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
