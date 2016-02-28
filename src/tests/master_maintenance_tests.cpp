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

#include <initializer_list>
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
#include <stout/hashset.hpp>
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

#include "master/allocator/mesos/allocator.hpp"

#include "slave/flags.hpp"

#include "tests/containerizer.hpp"
#include "tests/mesos.hpp"
#include "tests/utils.hpp"

using google::protobuf::RepeatedPtrField;

using mesos::internal::master::Master;

using mesos::internal::master::allocator::MesosAllocatorProcess;

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

using mesos::internal::protobuf::maintenance::createSchedule;
using mesos::internal::protobuf::maintenance::createUnavailability;
using mesos::internal::protobuf::maintenance::createWindow;

using std::string;
using std::vector;

using testing::AtMost;
using testing::DoAll;
using testing::Not;

namespace mesos {
namespace internal {
namespace tests {

JSON::Array createMachineList(std::initializer_list<MachineID> _ids)
{
  RepeatedPtrField<MachineID> ids =
    internal::protobuf::maintenance::createMachineList(_ids);

  return JSON::protobuf(ids);
}


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

  virtual master::Flags CreateMasterFlags()
  {
    master::Flags masterFlags = MesosTest::CreateMasterFlags();
    masterFlags.authenticate_frameworks = false;
    return masterFlags;
  }

  virtual slave::Flags CreateSlaveFlags()
  {
    slave::Flags slaveFlags = MesosTest::CreateSlaveFlags();
    slaveFlags.hostname = maintenanceHostname;
    return slaveFlags;
  }

  // Default headers for all POST's to maintenance endpoints.
  process::http::Headers headers;

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
    MOCK_METHOD0(connected, void());
    MOCK_METHOD0(disconnected, void());
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
      stringify(JSON::protobuf(schedule)));

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
      stringify(JSON::protobuf(schedule)));

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
      stringify(JSON::protobuf(schedule)));

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
      stringify(JSON::protobuf(schedule)));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);

  // Try to replace with another invalid schedule with duplicate machines.
  schedule = createSchedule({
      createWindow({machine1}, unavailability),
      createWindow({machine1}, unavailability)});

  response = process::http::post(
      master.get(),
      "maintenance/schedule",
      headers,
      stringify(JSON::protobuf(schedule)));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);

  // Try to replace with an invalid schedule with a badly formed MachineInfo.
  schedule = createSchedule(
      {createWindow({badMachine}, unavailability)});

  response = process::http::post(
      master.get(),
      "maintenance/schedule",
      headers,
      stringify(JSON::protobuf(schedule)));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);

  // Post a valid schedule with two machines.
  schedule = createSchedule(
      {createWindow({machine1, machine2}, unavailability)});

  response = process::http::post(
      master.get(),
      "maintenance/schedule",
      headers,
      stringify(JSON::protobuf(schedule)));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Delete the schedule (via an empty schedule).
  schedule = createSchedule({});
  response = process::http::post(
      master.get(),
      "maintenance/schedule",
      headers,
      stringify(JSON::protobuf(schedule)));

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
      stringify(JSON::protobuf(schedule)));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Deactivate machine1.
  JSON::Array machines = createMachineList({machine1});
  response = process::http::post(
      master.get(),
      "machine/down",
      headers,
      stringify(machines));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Try to unschedule machine1, by posting a schedule without it.
  schedule = createSchedule({createWindow({machine2}, unavailability)});

  response = process::http::post(
      master.get(),
      "maintenance/schedule",
      headers,
      stringify(JSON::protobuf(schedule)));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);

  // Reactivate machine1.
  machines = createMachineList({machine1});
  response = process::http::post(
      master.get(),
      "machine/up",
      headers,
      stringify(machines));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
}


// Test ensures that an offer will have an `unavailability` set if the
// slave is scheduled to go down for maintenance.
TEST_F(MasterMaintenanceTest, PendingUnavailabilityTest)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  Try<PID<Slave>> slave = StartSlave(&exec);
  ASSERT_SOME(slave);

  Callbacks callbacks;

  Future<Nothing> connected;
  EXPECT_CALL(callbacks, connected())
    .WillOnce(FutureSatisfy(&connected))
    .WillRepeatedly(Return()); // Ignore future invocations.

  Mesos mesos(
      master.get(),
      ContentType::PROTOBUF,
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
  machine.set_hostname(maintenanceHostname);
  machine.set_ip(stringify(slave.get().address.ip));

  const Time start = Clock::now() + Seconds(60);
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
      stringify(JSON::protobuf(schedule)));

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

  EXPECT_CALL(callbacks, disconnected())
    .Times(AtMost(1));

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
}


// Test ensures that old schedulers gracefully handle inverse offers, even if
// they aren't passed up to the top level API yet.
TEST_F(MasterMaintenanceTest, PreV1SchedulerSupport)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  Try<PID<Slave>> slave = StartSlave(&exec);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  // Intercept offers sent to the scheduler.
  Future<vector<Offer>> normalOffers;
  Future<vector<Offer>> unavailabilityOffers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&normalOffers))
    .WillOnce(FutureArg<1>(&unavailabilityOffers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  // The original offers should be rescinded when the unavailability is changed.
  Future<Nothing> offerRescinded;
  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .WillOnce(FutureSatisfy(&offerRescinded));

  // Start the test.
  driver.start();

  // Wait for some normal offers.
  AWAIT_READY(normalOffers);
  EXPECT_NE(0u, normalOffers.get().size());

  // Check that unavailability is not set.
  foreach (const Offer& offer, normalOffers.get()) {
    EXPECT_FALSE(offer.has_unavailability());
  }

  // Schedule this slave for maintenance.
  MachineID machine;
  machine.set_hostname(maintenanceHostname);
  machine.set_ip(stringify(slave.get().address.ip));

  const Time start = Clock::now() + Seconds(60);
  const Duration duration = Seconds(120);
  const Unavailability unavailability = createUnavailability(start, duration);

  // Post a valid schedule with one machine.
  maintenance::Schedule schedule = createSchedule(
      {createWindow({machine}, unavailability)});

  // We have a few seconds between the first set of offers and the next
  // allocation of offers. This should be enough time to perform a maintenance
  // schedule update. This update will also trigger the rescinding of offers
  // from the scheduled slave.
  Future<Response> response =
    process::http::post(
        master.get(),
        "maintenance/schedule",
        headers,
        stringify(JSON::protobuf(schedule)));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Wait for some offers.
  AWAIT_READY(unavailabilityOffers);
  EXPECT_NE(0u, unavailabilityOffers.get().size());

  // Check that each offer has an unavailability.
  foreach (const Offer& offer, unavailabilityOffers.get()) {
    EXPECT_TRUE(offer.has_unavailability());
    EXPECT_EQ(unavailability.start(), offer.unavailability().start());
    EXPECT_EQ(unavailability.duration(), offer.unavailability().duration());
  }

  driver.stop();
  driver.join();

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
}


// Test ensures that slaves receive a shutdown message from the master when
// maintenance is started, and frameworks receive a task lost message.
TEST_F(MasterMaintenanceTest, EnterMaintenanceMode)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  slave::Flags slaveFlags = CreateSlaveFlags();
  Try<PID<Slave>> slave = StartSlave(&exec, slaveFlags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  // Launch a task.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, 1, 64, "*"))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(1);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .WillRepeatedly(Return()); // Ignore rescinds.

  // Collect the status updates to verify the task is running and then lost.
  Future<TaskStatus> startStatus, lostStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&startStatus))
    .WillOnce(FutureArg<1>(&lostStatus));

  // Start the test.
  driver.start();

  // Wait till the task is running to schedule the maintenance.
  AWAIT_READY(startStatus);
  EXPECT_EQ(TASK_RUNNING, startStatus.get().state());

  // Schedule this slave for maintenance.
  MachineID machine;
  machine.set_hostname(maintenanceHostname);
  machine.set_ip(stringify(slave.get().address.ip));

  const Time start = Clock::now() + Seconds(60);
  const Duration duration = Seconds(120);
  const Unavailability unavailability = createUnavailability(start, duration);

  // Post a valid schedule with one machine.
  maintenance::Schedule schedule = createSchedule(
      {createWindow({machine}, unavailability)});

  // We have a few seconds between the first set of offers and the next
  // allocation of offers.  This should be enough time to perform a maintenance
  // schedule update.  This update will also trigger the rescinding of offers
  // from the scheduled slave.
  Future<Response> response =
    process::http::post(
        master.get(),
        "maintenance/schedule",
        headers,
        stringify(JSON::protobuf(schedule)));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Verify that the master forces the slave to be shut down after the
  // maintenance is started.
  Future<ShutdownMessage> shutdownMessage =
    FUTURE_PROTOBUF(ShutdownMessage(), master.get(), slave.get());

  // Verify that the framework will be informed that the slave is lost.
  Future<Nothing> slaveLost;
  EXPECT_CALL(sched, slaveLost(&driver, _))
    .WillOnce(FutureSatisfy(&slaveLost));

  // Start the maintenance.
  response =
    process::http::post(
        master.get(),
        "machine/down",
        headers,
        stringify(createMachineList({machine})));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Wait for the slave to be told to shut down.
  AWAIT_READY(shutdownMessage);

  // Verify that we received a TASK_LOST.
  AWAIT_READY(lostStatus);
  EXPECT_EQ(TASK_LOST, lostStatus.get().state());

  // Verify that the framework received the slave lost message.
  AWAIT_READY(slaveLost);

  Clock::pause();
  Clock::settle();
  Clock::advance(slaveFlags.executor_shutdown_grace_period);
  Clock::resume();

  // Wait on the agent to terminate so that it wipes out it's latest symlink.
  // This way when we launch a new agent it will register with a new agent id.
  wait(slave.get());

  // Ensure that the slave gets shut down immediately if it tries to register
  // from a machine that is under maintenance.
  shutdownMessage = FUTURE_PROTOBUF(ShutdownMessage(), master.get(), _);
  EXPECT_TRUE(shutdownMessage.isPending());

  slave = StartSlave();
  ASSERT_SOME(slave);

  AWAIT_READY(shutdownMessage);

  // Wait on the agent to terminate so that it wipes out it's latest symlink.
  // This way when we launch a new agent it will register with a new agent id.
  wait(slave.get());

  // Stop maintenance.
  response =
    process::http::post(
        master.get(),
        "machine/up",
        headers,
        stringify(createMachineList({machine})));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Capture the registration message.
  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  // Start the agent again.
  slave = StartSlave();

  // Wait for agent registration.
  AWAIT_READY(slaveRegisteredMessage);

  driver.stop();
  driver.join();

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
  JSON::Array machines = createMachineList({machine1, machine2});
  Future<Response> response = process::http::post(
      master.get(),
      "machine/down",
      headers,
      stringify(machines));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);

  // Try an empty list.
  machines = createMachineList({});
  response = process::http::post(
      master.get(),
      "machine/down",
      headers,
      stringify(machines));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);

  // Try an empty machine.
  machines = createMachineList({badMachine});
  response = process::http::post(
      master.get(),
      "machine/down",
      headers,
      stringify(machines));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);

  // Post a valid schedule with two machines.
  maintenance::Schedule schedule = createSchedule(
      {createWindow({machine1, machine2}, unavailability)});

  response = process::http::post(
      master.get(),
      "maintenance/schedule",
      headers,
      stringify(JSON::protobuf(schedule)));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Down machine1.
  machines = createMachineList({machine1});
  response = process::http::post(
      master.get(),
      "machine/down",
      headers,
      stringify(machines));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Fail to down machine1 again.
  response = process::http::post(
      master.get(),
      "machine/down",
      headers,
      stringify(machines));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);

  // Fail to down machine1 and machine2.
  machines = createMachineList({machine1, machine2});
  response = process::http::post(
      master.get(),
      "machine/down",
      headers,
      stringify(machines));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);

  // Down machine2.
  machines = createMachineList({machine2});
  response = process::http::post(
      master.get(),
      "machine/down",
      headers,
      stringify(machines));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
}


// Posts valid and invalid machines to the maintenance stop endpoint.
TEST_F(MasterMaintenanceTest, BringUpMachines)
{
  // Set up a master.
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Try to bring up an unscheduled machine.
  JSON::Array machines = createMachineList({machine1, machine2});
  Future<Response> response = process::http::post(
      master.get(),
      "machine/up",
      headers,
      stringify(machines));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);

  // Post a valid schedule with three machines.
  maintenance::Schedule schedule = createSchedule({
      createWindow({machine1, machine2}, unavailability),
      createWindow({machine3}, unavailability)});

  response = process::http::post(
      master.get(),
      "maintenance/schedule",
      headers,
      stringify(JSON::protobuf(schedule)));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Try to bring up a non-down machine.
  machines = createMachineList({machine1, machine2});
  response = process::http::post(
      master.get(),
      "machine/up",
      headers,
      stringify(machines));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);

  // Down machine3.
  machines = createMachineList({machine3});
  response = process::http::post(
      master.get(),
      "machine/down",
      headers,
      stringify(machines));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Up machine3.
  response = process::http::post(
      master.get(),
      "machine/up",
      headers,
      stringify(machines));

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
      stringify(machines));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Up the other machines.
  response = process::http::post(
      master.get(),
      "machine/up",
      headers,
      stringify(machines));

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
      stringify(JSON::protobuf(schedule)));

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
  JSON::Array machines = createMachineList({machine1});
  response = process::http::post(
      master.get(),
      "machine/down",
      headers,
      stringify(machines));

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
      stringify(machines));

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
  ASSERT_EQ("0.0.0.2", statuses.get().draining_machines(0).id().ip());
}


// Test ensures that accept and decline works with inverse offers.
// And that accepted/declined inverse offers will be reflected
// in the maintenance status endpoint.
TEST_F(MasterMaintenanceTest, InverseOffers)
{
  // Set up a master.
  master::Flags masterFlags = CreateMasterFlags();
  Try<PID<Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);

  Try<PID<Slave>> slave = StartSlave(&exec);
  ASSERT_SOME(slave);

  // Before starting any frameworks, put the one machine into `DRAINING` mode.
  MachineID machine;
  machine.set_hostname(maintenanceHostname);
  machine.set_ip(stringify(slave.get().address.ip));

  const Time start = Clock::now() + Seconds(60);
  const Duration duration = Seconds(120);
  const Unavailability unavailability = createUnavailability(start, duration);

  maintenance::Schedule schedule = createSchedule(
      {createWindow({machine}, unavailability)});

  Future<Response> response = process::http::post(
      master.get(),
      "maintenance/schedule",
      headers,
      stringify(JSON::protobuf(schedule)));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Sanity check that this machine shows up in the status endpoint
  // and there should be no inverse offer status.
  response = process::http::get(master.get(), "maintenance/status");
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  Try<JSON::Object> statuses_ = JSON::parse<JSON::Object>(response.get().body);
  ASSERT_SOME(statuses_);
  Try<maintenance::ClusterStatus> statuses =
    ::protobuf::parse<maintenance::ClusterStatus>(statuses_.get());

  ASSERT_SOME(statuses);
  ASSERT_EQ(0, statuses.get().down_machines().size());
  ASSERT_EQ(1, statuses.get().draining_machines().size());
  ASSERT_EQ(
      maintenanceHostname,
      statuses.get().draining_machines(0).id().hostname());
  ASSERT_EQ(0, statuses.get().draining_machines(0).statuses().size());

  // Now start a framework.
  Callbacks callbacks;

  Future<Nothing> connected;
  EXPECT_CALL(callbacks, connected())
    .WillOnce(FutureSatisfy(&connected))
    .WillRepeatedly(Return()); // Ignore future invocations.

  Mesos mesos(
      master.get(),
      ContentType::PROTOBUF,
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

  // Ensure we receive some regular resource offers.
  event = events.get();
  AWAIT_READY(event);
  EXPECT_EQ(Event::OFFERS, event.get().type());
  EXPECT_NE(0, event.get().offers().offers().size());
  EXPECT_EQ(0, event.get().offers().inverse_offers().size());

  // All the offers should have unavailability.
  foreach (const v1::Offer& offer, event.get().offers().offers()) {
    EXPECT_TRUE(offer.has_unavailability());
  }

  // Just work with a single offer to simplify the rest of the test.
  v1::Offer offer = event.get().offers().offers(0);

  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(1);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  // A dummy task just for confirming that the offer is accepted.
  // TODO(josephw): Replace with v1 API createTask helper.
  v1::TaskInfo taskInfo =
    evolve(createTask(devolve(offer), "", DEFAULT_EXECUTOR_ID));

  {
    // Accept this one offer.
    Call call;
    call.mutable_framework_id()->CopyFrom(id);
    call.set_type(Call::ACCEPT);

    Call::Accept* accept = call.mutable_accept();
    accept->add_offer_ids()->CopyFrom(offer.id());

    v1::Offer::Operation* operation = accept->add_operations();
    operation->set_type(v1::Offer::Operation::LAUNCH);
    operation->mutable_launch()->add_task_infos()->CopyFrom(taskInfo);

    mesos.send(call);
  }

  // Expect an inverse offer.
  event = events.get();
  AWAIT_READY(event);
  EXPECT_EQ(Event::OFFERS, event.get().type());
  EXPECT_EQ(0, event.get().offers().offers().size());
  EXPECT_EQ(1, event.get().offers().inverse_offers().size());

  // Save this inverse offer so we can decline it later.
  v1::InverseOffer inverseOffer = event.get().offers().inverse_offers(0);

  // Wait for the task to start running.
  event = events.get();
  AWAIT_READY(event);
  EXPECT_EQ(Event::UPDATE, event.get().type());
  EXPECT_EQ(v1::TASK_RUNNING, event.get().update().status().state());

  {
    // Acknowledge TASK_RUNNING update.
    Call call;
    call.mutable_framework_id()->CopyFrom(id);
    call.set_type(Call::ACKNOWLEDGE);

    Call::Acknowledge* acknowledge = call.mutable_acknowledge();
    acknowledge->mutable_task_id()->CopyFrom(taskInfo.task_id());
    acknowledge->mutable_agent_id()->CopyFrom(offer.agent_id());
    acknowledge->set_uuid(event.get().update().status().uuid());

    mesos.send(call);
  }

  // To ensure that the decline call has reached the allocator before
  // we advance the clock for the next batch allocation, we block on
  // the appropriate allocator interface method being dispatched.
  Future<Nothing> updateInverseOffer =
    FUTURE_DISPATCH(_, &MesosAllocatorProcess::updateInverseOffer);

  {
    // Decline an inverse offer, with a filter.
    Call call;
    call.mutable_framework_id()->CopyFrom(id);
    call.set_type(Call::DECLINE);

    Call::Decline* decline = call.mutable_decline();
    decline->add_offer_ids()->CopyFrom(inverseOffer.id());

    // Set a 0 second filter to immediately get another inverse offer.
    v1::Filters filters;
    filters.set_refuse_seconds(0);
    decline->mutable_filters()->CopyFrom(filters);

    mesos.send(call);
  }

  AWAIT_READY(updateInverseOffer);

  Clock::pause();
  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  // Expect another inverse offer.
  event = events.get();
  AWAIT_READY(event);
  EXPECT_EQ(Event::OFFERS, event.get().type());
  Clock::resume();

  EXPECT_EQ(0, event.get().offers().offers().size());
  EXPECT_EQ(1, event.get().offers().inverse_offers().size());
  inverseOffer = event.get().offers().inverse_offers(0);

  // Check that the status endpoint shows the inverse offer as declined.
  response = process::http::get(master.get(), "maintenance/status");
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  statuses_ = JSON::parse<JSON::Object>(response.get().body);
  ASSERT_SOME(statuses_);
  statuses = ::protobuf::parse<maintenance::ClusterStatus>(statuses_.get());

  ASSERT_SOME(statuses);
  ASSERT_EQ(0, statuses.get().down_machines().size());
  ASSERT_EQ(1, statuses.get().draining_machines().size());
  ASSERT_EQ(
      maintenanceHostname,
      statuses.get().draining_machines(0).id().hostname());

  ASSERT_EQ(1, statuses.get().draining_machines(0).statuses().size());
  ASSERT_EQ(
      mesos::master::InverseOfferStatus::DECLINE,
      statuses.get().draining_machines(0).statuses(0).status());

  ASSERT_EQ(
      id,
      evolve(statuses.get().draining_machines(0).statuses(0).framework_id()));

  updateInverseOffer =
    FUTURE_DISPATCH(_, &MesosAllocatorProcess::updateInverseOffer);

  {
    // Accept an inverse offer, with filter.
    Call call;
    call.mutable_framework_id()->CopyFrom(id);
    call.set_type(Call::ACCEPT);

    Call::Accept* accept = call.mutable_accept();
    accept->add_offer_ids()->CopyFrom(inverseOffer.id());

    // Set a 0 second filter to immediately get another inverse offer.
    v1::Filters filters;
    filters.set_refuse_seconds(0);
    accept->mutable_filters()->CopyFrom(filters);

    mesos.send(call);
  }

  AWAIT_READY(updateInverseOffer);

  Clock::pause();
  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  // Expect yet another inverse offer.
  event = events.get();
  AWAIT_READY(event);
  EXPECT_EQ(Event::OFFERS, event.get().type());
  Clock::resume();

  EXPECT_EQ(0, event.get().offers().offers().size());
  EXPECT_EQ(1, event.get().offers().inverse_offers().size());

  // Check that the status endpoint shows the inverse offer as accepted.
  response = process::http::get(master.get(), "maintenance/status");
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  statuses_ = JSON::parse<JSON::Object>(response.get().body);
  ASSERT_SOME(statuses_);
  statuses = ::protobuf::parse<maintenance::ClusterStatus>(statuses_.get());

  ASSERT_SOME(statuses);
  ASSERT_EQ(0, statuses.get().down_machines().size());
  ASSERT_EQ(1, statuses.get().draining_machines().size());
  ASSERT_EQ(
      maintenanceHostname,
      statuses.get().draining_machines(0).id().hostname());

  ASSERT_EQ(1, statuses.get().draining_machines(0).statuses().size());
  ASSERT_EQ(
      mesos::master::InverseOfferStatus::ACCEPT,
      statuses.get().draining_machines(0).statuses(0).status());

  ASSERT_EQ(
      id,
      evolve(statuses.get().draining_machines(0).statuses(0).framework_id()));

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  EXPECT_CALL(callbacks, disconnected())
    .Times(AtMost(1));

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
}


// Test ensures that inverse offers support filters.
TEST_F(MasterMaintenanceTest, InverseOffersFilters)
{
  // Set up a master.
  // NOTE: We don't use `StartMaster()` because we need to access these flags.
  master::Flags flags = CreateMasterFlags();

  Try<PID<Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  ExecutorInfo executor1 = CREATE_EXECUTOR_INFO("executor-1", "exit 1");
  ExecutorInfo executor2 = CREATE_EXECUTOR_INFO("executor-2", "exit 2");

  MockExecutor exec1(executor1.executor_id());
  MockExecutor exec2(executor2.executor_id());

  hashmap<ExecutorID, Executor*> execs;
  execs[executor1.executor_id()] = &exec1;
  execs[executor2.executor_id()] = &exec2;

  TestContainerizer containerizer(execs);

  EXPECT_CALL(exec1, registered(_, _, _, _))
    .Times(1);

  EXPECT_CALL(exec1, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  EXPECT_CALL(exec2, registered(_, _, _, _))
    .Times(1);

  EXPECT_CALL(exec2, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  // Capture the registration message for the first slave.
  Future<SlaveRegisteredMessage> slave1RegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get(), _);

  // We need two agents for this test.
  Try<PID<Slave>> slave1 = StartSlave(&containerizer);
  ASSERT_SOME(slave1);

  // We need to make sure the first slave registers before we schedule the
  // machine it is running on for maintenance.
  AWAIT_READY(slave1RegisteredMessage);

  // Capture the registration message for the second slave.
  Future<SlaveRegisteredMessage> slave2RegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get(), Not(slave1.get()));

  slave::Flags slaveFlags2 = MesosTest::CreateSlaveFlags();
  slaveFlags2.hostname = maintenanceHostname + "-2";

  Try<PID<Slave>> slave2 = StartSlave(&containerizer, slaveFlags2);
  ASSERT_SOME(slave2);

  // We need to make sure the second slave registers before we schedule the
  // machine it is running on for maintenance.
  AWAIT_READY(slave2RegisteredMessage);

  // Before starting any frameworks, put the first machine into `DRAINING` mode.
  MachineID machine1;
  machine1.set_hostname(maintenanceHostname);
  machine1.set_ip(stringify(slave1.get().address.ip));

  MachineID machine2;
  machine2.set_hostname(slaveFlags2.hostname.get());
  machine2.set_ip(stringify(slave2.get().address.ip));

  const Time start = Clock::now() + Seconds(60);
  const Duration duration = Seconds(120);
  const Unavailability unavailability = createUnavailability(start, duration);

  maintenance::Schedule schedule = createSchedule(
      {createWindow({machine1, machine2}, unavailability)});

  Future<Response> response = process::http::post(
      master.get(),
      "maintenance/schedule",
      headers,
      stringify(JSON::protobuf(schedule)));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Pause the clock before starting a framework.
  // This ensures deterministic offer-ing behavior during the test.
  Clock::pause();

  // Now start a framework.
  Callbacks callbacks;

  Future<Nothing> connected;
  EXPECT_CALL(callbacks, connected())
    .WillOnce(FutureSatisfy(&connected))
    .WillRepeatedly(Return()); // Ignore future invocations.

  Mesos mesos(
      master.get(),
      ContentType::PROTOBUF,
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

  // Trigger a batch allocation.
  Clock::advance(flags.allocation_interval);

  event = events.get();
  AWAIT_READY(event);
  EXPECT_EQ(Event::OFFERS, event.get().type());
  EXPECT_EQ(2, event.get().offers().offers().size());
  EXPECT_EQ(0, event.get().offers().inverse_offers().size());

  // All the offers should have unavailability.
  foreach (const v1::Offer& offer, event.get().offers().offers()) {
    EXPECT_TRUE(offer.has_unavailability());
  }

  // Save both offers.
  v1::Offer offer1 = event.get().offers().offers(0);
  v1::Offer offer2 = event.get().offers().offers(1);

  // Spawn dummy tasks using both offers.
  v1::TaskInfo taskInfo1 =
    evolve(createTask(devolve(offer1), "exit 1", executor1.executor_id()));

  v1::TaskInfo taskInfo2 =
    evolve(createTask(devolve(offer2), "exit 2", executor2.executor_id()));

  {
    // Accept the first offer.
    Call call;
    call.mutable_framework_id()->CopyFrom(id);
    call.set_type(Call::ACCEPT);

    Call::Accept* accept = call.mutable_accept();
    accept->add_offer_ids()->CopyFrom(offer1.id());

    v1::Offer::Operation* operation = accept->add_operations();
    operation->set_type(v1::Offer::Operation::LAUNCH);
    operation->mutable_launch()->add_task_infos()->CopyFrom(taskInfo1);

    mesos.send(call);
  }

  {
    // Accept the second offer.
    Call call;
    call.mutable_framework_id()->CopyFrom(id);
    call.set_type(Call::ACCEPT);

    Call::Accept* accept = call.mutable_accept();
    accept->add_offer_ids()->CopyFrom(offer2.id());

    v1::Offer::Operation* operation = accept->add_operations();
    operation->set_type(v1::Offer::Operation::LAUNCH);
    operation->mutable_launch()->add_task_infos()->CopyFrom(taskInfo2);

    mesos.send(call);
  }

  // Expect two inverse offers.
  event = events.get();
  AWAIT_READY(event);
  EXPECT_EQ(Event::OFFERS, event.get().type());
  EXPECT_EQ(0, event.get().offers().offers().size());
  EXPECT_EQ(2, event.get().offers().inverse_offers().size());

  // Save these inverse offers.
  v1::InverseOffer inverseOffer1 = event.get().offers().inverse_offers(0);
  v1::InverseOffer inverseOffer2 = event.get().offers().inverse_offers(1);

  // We want to acknowledge TASK_RUNNING updates for the two tasks we
  // have launched. We don't know which task will be launched first,
  // so just send acknowledgments in response to the TASK_RUNNING
  // events we receive. We track which task ids we acknowledge, and
  // then verify them with the expected task ids.
  hashset<v1::TaskID> acknowledgedTaskIds;
  event = events.get();
  AWAIT_READY(event);
  EXPECT_EQ(Event::UPDATE, event.get().type());

  v1::TaskStatus updateStatus = event.get().update().status();
  EXPECT_EQ(v1::TASK_RUNNING, updateStatus.state());

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(id);
    call.set_type(Call::ACKNOWLEDGE);

    Call::Acknowledge* acknowledge = call.mutable_acknowledge();
    acknowledge->mutable_task_id()->CopyFrom(updateStatus.task_id());
    acknowledge->mutable_agent_id()->CopyFrom(updateStatus.agent_id());
    acknowledge->set_uuid(updateStatus.uuid());

    EXPECT_FALSE(acknowledgedTaskIds.contains(updateStatus.task_id()));
    acknowledgedTaskIds.insert(updateStatus.task_id());

    mesos.send(call);
  }

  event = events.get();
  AWAIT_READY(event);
  EXPECT_EQ(Event::UPDATE, event.get().type());

  updateStatus = event.get().update().status();
  EXPECT_EQ(v1::TASK_RUNNING, updateStatus.state());

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(id);
    call.set_type(Call::ACKNOWLEDGE);

    Call::Acknowledge* acknowledge = call.mutable_acknowledge();
    acknowledge->mutable_task_id()->CopyFrom(updateStatus.task_id());
    acknowledge->mutable_agent_id()->CopyFrom(updateStatus.agent_id());
    acknowledge->set_uuid(updateStatus.uuid());

    EXPECT_FALSE(acknowledgedTaskIds.contains(updateStatus.task_id()));
    acknowledgedTaskIds.insert(updateStatus.task_id());

    mesos.send(call);
  }

  // Now we can verify that we acknowledged the expected task ids.
  EXPECT_TRUE(acknowledgedTaskIds.contains(taskInfo1.task_id()));
  EXPECT_TRUE(acknowledgedTaskIds.contains(taskInfo2.task_id()));

  // To ensure that the accept call has reached the allocator before
  // we advance the clock for the next batch allocation, we block on
  // the appropriate allocator interface method being dispatched.
  Future<Nothing> updateInverseOffer =
    FUTURE_DISPATCH(_, &MesosAllocatorProcess::updateInverseOffer);

  {
    // Decline the second inverse offer, with a filter set such that
    // we should not see this inverse offer in subsequent batch
    // allocations for the remainder of this test.
    Call call;
    call.mutable_framework_id()->CopyFrom(id);
    call.set_type(Call::DECLINE);

    Call::Decline* decline = call.mutable_decline();
    decline->add_offer_ids()->CopyFrom(inverseOffer2.id());

    v1::Filters filters;
    filters.set_refuse_seconds(flags.allocation_interval.secs() + 100);
    decline->mutable_filters()->CopyFrom(filters);

    mesos.send(call);
  }

  AWAIT_READY(updateInverseOffer);

  // Accept the first inverse offer, with a filter set such that we
  // should see this inverse offer again in the next batch
  // allocation.
  updateInverseOffer =
    FUTURE_DISPATCH(_, &MesosAllocatorProcess::updateInverseOffer);

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(id);
    call.set_type(Call::ACCEPT);

    Call::Accept* accept = call.mutable_accept();
    accept->add_offer_ids()->CopyFrom(inverseOffer1.id());

    v1::Filters filters;
    filters.set_refuse_seconds(0);
    accept->mutable_filters()->CopyFrom(filters);

    mesos.send(call);
  }

  AWAIT_READY(updateInverseOffer);
  Clock::settle();
  Clock::advance(flags.allocation_interval);

  // Expect one inverse offer in this batch allocation.
  event = events.get();
  AWAIT_READY(event);

  EXPECT_EQ(Event::OFFERS, event.get().type());
  EXPECT_EQ(0, event.get().offers().offers().size());
  EXPECT_EQ(1, event.get().offers().inverse_offers().size());
  EXPECT_EQ(
      inverseOffer1.agent_id(),
      event.get().offers().inverse_offers(0).agent_id());

  inverseOffer1 = event.get().offers().inverse_offers(0);

  updateInverseOffer =
    FUTURE_DISPATCH(_, &MesosAllocatorProcess::updateInverseOffer);

  {
    // Do another immediate filter, but decline it this time.
    Call call;
    call.mutable_framework_id()->CopyFrom(id);
    call.set_type(Call::DECLINE);

    Call::Decline* decline = call.mutable_decline();
    decline->add_offer_ids()->CopyFrom(inverseOffer1.id());

    v1::Filters filters;
    filters.set_refuse_seconds(0);
    decline->mutable_filters()->CopyFrom(filters);

    mesos.send(call);
  }

  AWAIT_READY(updateInverseOffer);
  Clock::settle();
  Clock::advance(flags.allocation_interval);

  // Expect the same inverse offer in this batch allocation.
  event = events.get();
  AWAIT_READY(event);

  EXPECT_EQ(Event::OFFERS, event.get().type());
  EXPECT_EQ(0, event.get().offers().offers().size());
  EXPECT_EQ(1, event.get().offers().inverse_offers().size());
  EXPECT_EQ(
      inverseOffer1.agent_id(),
      event.get().offers().inverse_offers(0).agent_id());

  EXPECT_CALL(exec1, shutdown(_))
    .Times(AtMost(1));

  EXPECT_CALL(exec2, shutdown(_))
    .Times(AtMost(1));

  EXPECT_CALL(callbacks, disconnected())
    .Times(AtMost(1));

  Shutdown(); // Must shutdown before 'containerizer' gets deallocated.
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
