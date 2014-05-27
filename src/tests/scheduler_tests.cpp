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

#include <vector>

#include <mesos/scheduler.hpp>

#include <process/future.hpp>
#include <process/gtest.hpp>
#include <process/http.hpp>
#include <process/pid.hpp>

#include <process/metrics/metrics.hpp>

#include <stout/json.hpp>
#include <stout/try.hpp>

#include "master/master.hpp"

#include "tests/mesos.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::tests;

using mesos::internal::master::Master;

using process::Future;
using process::PID;

using process::http::OK;

using process::metrics::internal::MetricsProcess;

using std::vector;

using testing::_;
using testing::Return;


class SchedulerTest : public MesosTest {};


TEST_F(SchedulerTest, MetricsEndpoint)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  ASSERT_EQ(DRIVER_RUNNING, driver.start());

  Future<process::http::Response> response =
    process::http::get(MetricsProcess::instance()->self(), "/snapshot");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  EXPECT_SOME_EQ(
      "application/json",
      response.get().headers.get("Content-Type"));

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response.get().body);

  ASSERT_SOME(parse);

  JSON::Object metrics = parse.get();

  EXPECT_EQ(1u, metrics.values.count("scheduler/event_queue_size"));

  driver.stop();
  driver.join();

  Shutdown();
}
