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

#include <string>

#include <mesos/maintenance/maintenance.hpp>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/http.hpp>
#include <process/pid.hpp>
#include <process/time.hpp>

#include <stout/duration.hpp>
#include <stout/json.hpp>
#include <stout/net.hpp>
#include <stout/option.hpp>
#include <stout/protobuf.hpp>
#include <stout/strings.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>

#include "common/protobuf_utils.hpp"

#include "master/master.hpp"

#include "slave/flags.hpp"

#include "tests/mesos.hpp"
#include "tests/utils.hpp"

using mesos::internal::master::Master;

using mesos::internal::slave::Slave;

using process::Clock;
using process::Future;
using process::PID;
using process::Time;

using process::http::BadRequest;
using process::http::OK;
using process::http::Response;

using mesos::internal::protobuf::maintenance::createMachineList;
using mesos::internal::protobuf::maintenance::createSchedule;
using mesos::internal::protobuf::maintenance::createUnavailability;
using mesos::internal::protobuf::maintenance::createWindow;

using std::string;

using testing::DoAll;

namespace mesos {
namespace internal {
namespace tests {

class MasterMaintenanceTest : public MesosTest
{
public:
  virtual void SetUp()
  {
    MesosTest::SetUp();

    // Initialize the default POST header.
    headers["Content-Type"] = "application/json";

    // Initialize some `MachineID`s.
    machine1.set_hostname("Machine1");
    machine2.set_ip("0.0.0.2");
    machine3.set_hostname("Machine3");
    machine3.set_ip("0.0.0.3");

    // Initialize the default `Unavailability`.
    unavailability = createUnavailability(Clock::now());
  }


  // Default headers for all POST's to maintenance endpoints.
  hashmap<string, string> headers;

  // Some generic `MachineID`s that can be used in this test.
  MachineID machine1;
  MachineID machine2;
  MachineID machine3;

  // Default unavailability.  Used when the test does not care
  // about the value of the unavailability.
  Unavailability unavailability;
};


// Posts valid and invalid schedules to the maintenance schedule endpoint.
TEST_F(MasterMaintenanceTest, UpdateSchedule)
{
  // Set up a master.
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Extra machine used in this test.
  // It isn't filled in, so it's incorrect.
  MachineID badMachine;

  // Post a valid schedule with one machine.
  maintenance::Schedule schedule = createSchedule(
      {createWindow({machine1}, unavailability)});

  Future<Response> response = process::http::post(
      master.get(),
      "maintenance/schedule",
      headers,
      stringify(JSON::Protobuf(schedule)));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Get the maintenance schedule.
  response = process::http::get(
      master.get(),
      "maintenance/schedule");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Check that the schedule was saved.
  Try<JSON::Object> masterSchedule_ =
    JSON::parse<JSON::Object>(response.get().body);

  ASSERT_SOME(masterSchedule_);
  Try<mesos::maintenance::Schedule> masterSchedule =
    ::protobuf::parse<mesos::maintenance::Schedule>(masterSchedule_.get());

  ASSERT_SOME(masterSchedule);
  ASSERT_EQ(1, masterSchedule.get().windows().size());
  ASSERT_EQ(1, masterSchedule.get().windows(0).machine_ids().size());
  ASSERT_EQ(
      "Machine1",
      masterSchedule.get().windows(0).machine_ids(0).hostname());

  // Try to replace with an invalid schedule with an empty window.
  schedule = createSchedule(
      {createWindow({}, unavailability)});

  response = process::http::post(
      master.get(),
      "maintenance/schedule",
      headers,
      stringify(JSON::Protobuf(schedule)));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);

  // Update the schedule with an unavailability with negative time.
  schedule = createSchedule({
      createWindow(
          {machine1},
          createUnavailability(Time::create(-10).get()))});

  response = process::http::post(
      master.get(),
      "maintenance/schedule",
      headers,
      stringify(JSON::Protobuf(schedule)));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Try to replace with an invalid schedule with a negative duration.
  schedule = createSchedule({
      createWindow(
          {machine1},
          createUnavailability(Clock::now(), Seconds(-10)))});

  response = process::http::post(
      master.get(),
      "maintenance/schedule",
      headers,
      stringify(JSON::Protobuf(schedule)));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);

  // Try to replace with another invalid schedule with duplicate machines.
  schedule = createSchedule({
      createWindow({machine1}, unavailability),
      createWindow({machine1}, unavailability)});

  response = process::http::post(
      master.get(),
      "maintenance/schedule",
      headers,
      stringify(JSON::Protobuf(schedule)));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);

  // Try to replace with an invalid schedule with a badly formed MachineInfo.
  schedule = createSchedule(
      {createWindow({badMachine}, unavailability)});

  response = process::http::post(
      master.get(),
      "maintenance/schedule",
      headers,
      stringify(JSON::Protobuf(schedule)));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);

  // Post a valid schedule with two machines.
  schedule = createSchedule(
      {createWindow({machine1, machine2}, unavailability)});

  response = process::http::post(
      master.get(),
      "maintenance/schedule",
      headers,
      stringify(JSON::Protobuf(schedule)));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Delete the schedule (via an empty schedule).
  schedule = createSchedule({});
  response = process::http::post(
      master.get(),
      "maintenance/schedule",
      headers,
      stringify(JSON::Protobuf(schedule)));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
}


// Posts valid and invalid machines to the maintenance start endpoint.
TEST_F(MasterMaintenanceTest, BringDownMachines)
{
  // Set up a master.
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Extra machine used in this test.
  // It isn't filled in, so it's incorrect.
  MachineID badMachine;

  // Try to start maintenance on an unscheduled machine.
  MachineIDs machines = createMachineList({machine1, machine2});
  Future<Response> response = process::http::post(
      master.get(),
      "machine/down",
      headers,
      stringify(JSON::Protobuf(machines)));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);

  // Try an empty list.
  machines = createMachineList({});
  response = process::http::post(
      master.get(),
      "machine/down",
      headers,
      stringify(JSON::Protobuf(machines)));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);

  // Try an empty machine.
  machines = createMachineList({badMachine});
  response = process::http::post(
      master.get(),
      "machine/down",
      headers,
      stringify(JSON::Protobuf(machines)));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);

  // Post a valid schedule with two machines.
  maintenance::Schedule schedule = createSchedule(
      {createWindow({machine1, machine2}, unavailability)});

  response = process::http::post(
      master.get(),
      "maintenance/schedule",
      headers,
      stringify(JSON::Protobuf(schedule)));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Down machine1.
  machines = createMachineList({machine1});
  response = process::http::post(
      master.get(),
      "machine/down",
      headers,
      stringify(JSON::Protobuf(machines)));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Fail to down machine1 again.
  response = process::http::post(
      master.get(),
      "machine/down",
      headers,
      stringify(JSON::Protobuf(machines)));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);

  // Fail to down machine1 and machine2.
  machines = createMachineList({machine1, machine2});
  response = process::http::post(
      master.get(),
      "machine/down",
      headers,
      stringify(JSON::Protobuf(machines)));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);

  // Down machine2.
  machines = createMachineList({machine2});
  response = process::http::post(
      master.get(),
      "machine/down",
      headers,
      stringify(JSON::Protobuf(machines)));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
}


// Posts valid and invalid machines to the maintenance stop endpoint.
TEST_F(MasterMaintenanceTest, BringUpMachines)
{
  // Set up a master.
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Try to bring up an unscheduled machine.
  MachineIDs machines = createMachineList({machine1, machine2});
  Future<Response> response = process::http::post(
      master.get(),
      "machine/up",
      headers,
      stringify(JSON::Protobuf(machines)));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);

  // Post a valid schedule with three machines.
  maintenance::Schedule schedule = createSchedule({
      createWindow({machine1, machine2}, unavailability),
      createWindow({machine3}, unavailability)});

  response = process::http::post(
      master.get(),
      "maintenance/schedule",
      headers,
      stringify(JSON::Protobuf(schedule)));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Try to bring up a non-down machine.
  machines = createMachineList({machine1, machine2});
  response = process::http::post(
      master.get(),
      "machine/up",
      headers,
      stringify(JSON::Protobuf(machines)));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);

  // Down machine3.
  machines = createMachineList({machine3});
  response = process::http::post(
      master.get(),
      "machine/down",
      headers,
      stringify(JSON::Protobuf(machines)));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Up machine3.
  response = process::http::post(
      master.get(),
      "machine/up",
      headers,
      stringify(JSON::Protobuf(machines)));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Get the maintenance schedule.
  response = process::http::get(
      master.get(),
      "maintenance/schedule");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Check that only one maintenance window remains.
  Try<JSON::Object> masterSchedule_ =
    JSON::parse<JSON::Object>(response.get().body);

  ASSERT_SOME(masterSchedule_);
  Try<mesos::maintenance::Schedule> masterSchedule =
    ::protobuf::parse<mesos::maintenance::Schedule>(masterSchedule_.get());

  ASSERT_SOME(masterSchedule);
  ASSERT_EQ(1, masterSchedule.get().windows().size());
  ASSERT_EQ(2, masterSchedule.get().windows(0).machine_ids().size());

  // Down the other machines.
  machines = createMachineList({machine1, machine2});
  response = process::http::post(
      master.get(),
      "machine/down",
      headers,
      stringify(JSON::Protobuf(machines)));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Up the other machines.
  response = process::http::post(
      master.get(),
      "machine/up",
      headers,
      stringify(JSON::Protobuf(machines)));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Get the maintenance schedule again.
  response = process::http::get(
      master.get(),
      "maintenance/schedule");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Check that the schedule is empty.
  masterSchedule_ = JSON::parse<JSON::Object>(response.get().body);

  ASSERT_SOME(masterSchedule_);
  masterSchedule =
    ::protobuf::parse<mesos::maintenance::Schedule>(masterSchedule_.get());

  ASSERT_SOME(masterSchedule);
  ASSERT_EQ(0, masterSchedule.get().windows().size());
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
