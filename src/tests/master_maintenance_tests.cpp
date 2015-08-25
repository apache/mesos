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

#include <mesos/v1/mesos.hpp>
#include <mesos/v1/resources.hpp>
#include <mesos/v1/scheduler.hpp>

#include <mesos/v1/scheduler/scheduler.hpp>

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

#include "internal/devolve.hpp"
#include "internal/evolve.hpp"

#include "master/master.hpp"

#include "slave/flags.hpp"

#include "tests/mesos.hpp"
#include "tests/utils.hpp"

using mesos::internal::master::Master;

using mesos::internal::slave::Slave;

using mesos::v1::scheduler::Call;
using mesos::v1::scheduler::Event;
using mesos::v1::scheduler::Mesos;

using process::Clock;
using process::Future;
using process::PID;
using process::Queue;
using process::Time;

using process::http::BadRequest;
using process::http::OK;
using process::http::Response;

using mesos::internal::protobuf::maintenance::createMachineList;
using mesos::internal::protobuf::maintenance::createSchedule;
using mesos::internal::protobuf::maintenance::createUnavailability;
using mesos::internal::protobuf::maintenance::createWindow;

using std::string;
using std::vector;

using testing::AtMost;
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

  virtual slave::Flags CreateSlaveFlags()
  {
    slave::Flags slaveFlags = MesosTest::CreateSlaveFlags();
    slaveFlags.hostname = maintenanceHostname;
    return slaveFlags;
  }

  // Default headers for all POST's to maintenance endpoints.
  hashmap<string, string> headers;

  const string maintenanceHostname = "maintenance-host";

  // Some generic `MachineID`s that can be used in this test.
  MachineID machine1;
  MachineID machine2;
  MachineID machine3;

  // Default unavailability.  Used when the test does not care
  // about the value of the unavailability.
  Unavailability unavailability;

protected:
  // Helper class for using EXPECT_CALL since the Mesos scheduler API
  // is callback based.
  class Callbacks
  {
  public:
    MOCK_METHOD0(connected, void(void));
    MOCK_METHOD0(disconnected, void(void));
    MOCK_METHOD1(received, void(const std::queue<Event>&));
  };
};


// Enqueues all received events into a libprocess queue.
// TODO(jmlvanre): Factor this common code out of tests into V1
// helper.
ACTION_P(Enqueue, queue)
{
  std::queue<Event> events = arg0;
  while (!events.empty()) {
    // Note that we currently drop HEARTBEATs because most of these tests
    // are not designed to deal with heartbeats.
    // TODO(vinod): Implement DROP_HTTP_CALLS that can filter heartbeats.
    if (events.front().type() == Event::HEARTBEAT) {
      VLOG(1) << "Ignoring HEARTBEAT event";
    } else {
      queue->put(events.front());
    }
    events.pop();
  }
}


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


// Tries to remove deactivated machines from the schedule.
TEST_F(MasterMaintenanceTest, FailToUnscheduleDeactivatedMachines)
{
  // Set up a master.
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Schedule two machines.
  maintenance::Schedule schedule = createSchedule(
      {createWindow({machine1, machine2}, unavailability)});

  Future<Response> response = process::http::post(
      master.get(),
      "maintenance/schedule",
      headers,
      stringify(JSON::Protobuf(schedule)));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Deactivate machine1.
  MachineIDs machines = createMachineList({machine1});
  response = process::http::post(
      master.get(),
      "machine/down",
      headers,
      stringify(JSON::Protobuf(machines)));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Try to unschedule machine1, by posting a schedule without it.
  schedule = createSchedule({createWindow({machine2}, unavailability)});

  response = process::http::post(
      master.get(),
      "maintenance/schedule",
      headers,
      stringify(JSON::Protobuf(schedule)));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);

  // Reactivate machine1.
  machines = createMachineList({machine1});
  response = process::http::post(
      master.get(),
      "machine/up",
      headers,
      stringify(JSON::Protobuf(machines)));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
}


// Test ensures that an offer will have an `unavailability` set if the
// slave is scheduled to go down for maintenance.
TEST_F(MasterMaintenanceTest, PendingUnavailabilityTest)
{
  master::Flags flags = CreateMasterFlags();
  flags.authenticate_frameworks = false;

  Try<PID<Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  Try<PID<Slave>> slave = StartSlave(&exec);
  ASSERT_SOME(slave);

  Callbacks callbacks;

  Future<Nothing> connected;
  EXPECT_CALL(callbacks, connected())
    .WillOnce(FutureSatisfy(&connected));

  Mesos mesos(
      master.get(),
      lambda::bind(&Callbacks::connected, lambda::ref(callbacks)),
      lambda::bind(&Callbacks::disconnected, lambda::ref(callbacks)),
      lambda::bind(&Callbacks::received, lambda::ref(callbacks), lambda::_1));

  AWAIT_READY(connected);

  Queue<Event> events;

  EXPECT_CALL(callbacks, received(_))
    .WillRepeatedly(Enqueue(&events));

  {
    Call call;
    call.set_type(Call::SUBSCRIBE);

    Call::Subscribe* subscribe = call.mutable_subscribe();
    subscribe->mutable_framework_info()->CopyFrom(DEFAULT_V1_FRAMEWORK_INFO);

    mesos.send(call);
  }

  Future<Event> event = events.get();
  AWAIT_READY(event);
  EXPECT_EQ(Event::SUBSCRIBED, event.get().type());

  v1::FrameworkID id(event.get().subscribed().framework_id());

  event = events.get();
  AWAIT_READY(event);
  EXPECT_EQ(Event::OFFERS, event.get().type());
  EXPECT_NE(0, event.get().offers().offers().size());
  const size_t numberOfOffers = event.get().offers().offers().size();

  // Regular offers shouldn't have unavailability.
  foreach (const v1::Offer& offer, event.get().offers().offers()) {
    EXPECT_FALSE(offer.has_unavailability());
  }

  // Schedule this slave for maintenance.
  MachineID machine;
  machine.set_hostname("maintenance-host");
  machine.set_ip(stringify(slave.get().address.ip));

  // TODO(jmlvanre): Replace Time(0.0) with `Clock::now()` once JSON double
  // conversion is fixed. For now using a rounded time avoids the issue.
  const Time start = Time::create(0.0).get() + Seconds(60);
  const Duration duration = Seconds(120);
  const Unavailability unavailability = createUnavailability(start, duration);

  // Post a valid schedule with one machine.
  maintenance::Schedule schedule = createSchedule(
      {createWindow({machine}, unavailability)});

  // We have a few seconds between the first set of offers and the
  // next allocation of offers.  This should be enough time to perform
  // a maintenance schedule update.  This update will also trigger the
  // rescinding of offers from the scheduled slave.
  Future<Response> response = process::http::post(
      master.get(),
      "maintenance/schedule",
      headers,
      stringify(JSON::Protobuf(schedule)));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // The original offers should be rescinded when the unavailability
  // is changed. We expect as many rescind events as we received
  // original offers.
  for (size_t offerNumber = 0; offerNumber < numberOfOffers; ++offerNumber) {
    event = events.get();
    AWAIT_READY(event);
    EXPECT_EQ(Event::RESCIND, event.get().type());
  }

  event = events.get();
  AWAIT_READY(event);
  EXPECT_EQ(Event::OFFERS, event.get().type());
  EXPECT_NE(0, event.get().offers().offers().size());

  // Make sure the new offers have the unavailability set.
  foreach (const v1::Offer& offer, event.get().offers().offers()) {
    EXPECT_TRUE(offer.has_unavailability());
    EXPECT_EQ(
        unavailability.start().nanoseconds(),
        offer.unavailability().start().nanoseconds());

    EXPECT_EQ(
        unavailability.duration().nanoseconds(),
        offer.unavailability().duration().nanoseconds());
  }

  // We also expect an inverse offer for the slave to go under
  // maintenance.
  event = events.get();
  AWAIT_READY(event);
  EXPECT_EQ(Event::OFFERS, event.get().type());
  EXPECT_NE(0, event.get().offers().inverse_offers().size());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
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


// Queries for machine statuses in between maintenance mode transitions.
TEST_F(MasterMaintenanceTest, MachineStatus)
{
  // Set up a master.
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Try to stop maintenance on an unscheduled machine.
  maintenance::Schedule schedule = createSchedule(
      {createWindow({machine1, machine2}, unavailability)});

  Future<Response> response = process::http::post(
      master.get(),
      "maintenance/schedule",
      headers,
      stringify(JSON::Protobuf(schedule)));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Get the maintenance statuses.
  response = process::http::get(master.get(), "maintenance/status");
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Check that both machines are draining.
  Try<JSON::Object> statuses_ =
    JSON::parse<JSON::Object>(response.get().body);

  ASSERT_SOME(statuses_);
  Try<maintenance::ClusterStatus> statuses =
    ::protobuf::parse<maintenance::ClusterStatus>(statuses_.get());

  ASSERT_SOME(statuses);
  ASSERT_EQ(2, statuses.get().draining_machines().size());
  ASSERT_EQ(0, statuses.get().down_machines().size());

  // Deactivate machine1.
  MachineIDs machines = createMachineList({machine1});
  response = process::http::post(
      master.get(),
      "machine/down",
      headers,
      stringify(JSON::Protobuf(machines)));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Get the maintenance statuses.
  response = process::http::get(master.get(), "maintenance/status");
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Check one machine is deactivated.
  statuses_ = JSON::parse<JSON::Object>(response.get().body);

  ASSERT_SOME(statuses_);
  statuses = ::protobuf::parse<maintenance::ClusterStatus>(statuses_.get());

  ASSERT_SOME(statuses);
  ASSERT_EQ(1, statuses.get().draining_machines().size());
  ASSERT_EQ(1, statuses.get().down_machines().size());
  ASSERT_EQ("Machine1", statuses.get().down_machines(0).hostname());

  // Reactivate machine1.
  machines = createMachineList({machine1});
  response = process::http::post(
      master.get(),
      "machine/up",
      headers,
      stringify(JSON::Protobuf(machines)));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Get the maintenance statuses.
  response = process::http::get(master.get(), "maintenance/status");
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Check that only one machine remains.
  statuses_ = JSON::parse<JSON::Object>(response.get().body);

  ASSERT_SOME(statuses_);
  statuses = ::protobuf::parse<maintenance::ClusterStatus>(statuses_.get());

  ASSERT_SOME(statuses);
  ASSERT_EQ(1, statuses.get().draining_machines().size());
  ASSERT_EQ(0, statuses.get().down_machines().size());
  ASSERT_EQ("0.0.0.2", statuses.get().draining_machines(0).ip());
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
