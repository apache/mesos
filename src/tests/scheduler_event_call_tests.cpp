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

#include <gmock/gmock.h>

#include <mesos/scheduler.hpp>

#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/message.hpp>
#include <process/pid.hpp>

#include <stout/nothing.hpp>
#include <stout/try.hpp>

#include "master/master.hpp"

#include "tests/containerizer.hpp"
#include "tests/mesos.hpp"

using mesos::internal::master::Master;

using mesos::scheduler::Event;

using process::Future;
using process::Message;
using process::PID;
using process::UPID;

using testing::_;
using testing::Eq;

namespace mesos {
namespace internal {
namespace tests {


// These tests intercept master messages and manually
// post Event messages to the driver.
class SchedulerDriverEventTest : public MesosTest {};


// Ensures that the driver can handle the ERROR event.
TEST_F(SchedulerDriverEventTest, Error)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<Message> frameworkRegisteredMessage =
    FUTURE_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()), _, _);

  driver.start();

  AWAIT_READY(frameworkRegisteredMessage);
  UPID frameworkPid = frameworkRegisteredMessage.get().to;

  Event event;
  event.set_type(Event::ERROR);
  event.mutable_error()->set_message("error message");

  Future<Nothing> error;
  EXPECT_CALL(sched, error(&driver, event.error().message()))
    .WillOnce(FutureSatisfy(&error));

  process::post(master.get(), frameworkPid, event);

  AWAIT_READY(error);
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
