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

#include <process/pid.hpp>

#include "tests/mesos.hpp"

using mesos::internal::master::Master;
using mesos::internal::slave::Slave;

using process::Future;
using process::PID;

using std::vector;

namespace mesos {
namespace internal {
namespace tests {

class RoleTest : public MesosTest {};


// This test checks that a framework cannot register with a role that
// is not in the configured list.
TEST_F(RoleTest, BadRegister)
{
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_role("invalid");

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.roles = "foo,bar";

  Try<PID<Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  Future<Nothing> error;
  EXPECT_CALL(sched, error(&driver, _))
    .WillOnce(FutureSatisfy(&error));

  driver.start();

  // Scheduler should get error message from the master.
  AWAIT_READY(error);

  driver.stop();
  driver.join();

  Shutdown();
}


// This test checks that the "/roles" endpoint returns the expected
// information when there are no active roles.
TEST_F(RoleTest, EndpointEmpty)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<process::http::Response> response =
    process::http::get(master.get(), "roles");

  AWAIT_READY(response);

  EXPECT_SOME_EQ(
      "application/json",
      response.get().headers.get("Content-Type"));

  Try<JSON::Value> parse = JSON::parse(response.get().body);
  ASSERT_SOME(parse);

  Try<JSON::Value> expected = JSON::parse(
      "{"
      "  \"roles\": ["
      "    {"
      "      \"frameworks\": [],"
      "      \"name\": \"*\","
      "      \"resources\": {"
      "        \"cpus\": 0,"
      "        \"disk\": 0,"
      "        \"mem\":  0"
      "      },"
      "      \"weight\": 1.0"
      "    }"
      "  ]"
      "}");

  ASSERT_SOME(expected);
  EXPECT_EQ(expected.get(), parse.get());

  Shutdown();
}


// This test checks that the "/roles" endpoint returns the expected
// information when there are configured weights and explicit roles,
// but no active frameworks.
TEST_F(RoleTest, EndpointNoFrameworks)
{
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.roles = "role1,role2";
  masterFlags.weights = "role1=5";

  Try<PID<Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Future<process::http::Response> response =
    process::http::get(master.get(), "roles");

  AWAIT_READY(response);

  EXPECT_SOME_EQ(
      "application/json",
      response.get().headers.get("Content-Type"));

  Try<JSON::Value> parse = JSON::parse(response.get().body);
  ASSERT_SOME(parse);

  Try<JSON::Value> expected = JSON::parse(
      "{"
      "  \"roles\": ["
      "    {"
      "      \"frameworks\": [],"
      "      \"name\": \"*\","
      "      \"resources\": {"
      "        \"cpus\": 0,"
      "        \"disk\": 0,"
      "        \"mem\":  0"
      "      },"
      "      \"weight\": 1.0"
      "    },"
      "    {"
      "      \"frameworks\": [],"
      "      \"name\": \"role1\","
      "      \"resources\": {"
      "        \"cpus\": 0,"
      "        \"disk\": 0,"
      "        \"mem\":  0"
      "      },"
      "      \"weight\": 5.0"
      "    },"
      "    {"
      "      \"frameworks\": [],"
      "      \"name\": \"role2\","
      "      \"resources\": {"
      "        \"cpus\": 0,"
      "        \"disk\": 0,"
      "        \"mem\":  0"
      "      },"
      "      \"weight\": 1.0"
      "    }"
      "  ]"
      "}");

  ASSERT_SOME(expected);
  EXPECT_EQ(expected.get(), parse.get());

  Shutdown();
}


}  // namespace tests {
}  // namespace internal {
}  // namespace mesos {
