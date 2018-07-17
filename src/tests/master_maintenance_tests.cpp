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

using mesos::master::detector::MasterDetector;

using mesos::v1::scheduler::Call;
using mesos::v1::scheduler::Event;
using mesos::v1::scheduler::Mesos;

using process::Clock;
using process::Future;
using process::Message;
using process::Owned;
using process::PID;
using process::Time;
using process::UPID;

using process::http::BadRequest;
using process::http::Forbidden;
using process::http::OK;
using process::http::Response;
using process::http::Unauthorized;

using mesos::internal::protobuf::maintenance::createSchedule;
using mesos::internal::protobuf::maintenance::createUnavailability;
using mesos::internal::protobuf::maintenance::createWindow;

using std::string;
using std::vector;

using testing::AtMost;
using testing::DoAll;
using testing::Eq;
using testing::Not;

namespace mesos {
namespace internal {
namespace tests {

JSON::Array createMachineList(std::initializer_list<MachineID> _ids)
{
  RepeatedPtrField<MachineID> ids =
    mesos::internal::protobuf::maintenance::createMachineList(_ids);

  return JSON::protobuf(ids);
}


class MasterMaintenanceTest : public MesosTest
{
public:
  void SetUp() override
  {
    MesosTest::SetUp();

    // Initialize the default POST header.
    headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
    headers["Content-Type"] = "application/json";

    // Initialize some `MachineID`s.
    machine1.set_hostname("Machine1");
    machine2.set_ip("0.0.0.2");
    machine3.set_hostname("Machine3");
    machine3.set_ip("0.0.0.3");

    // Initialize the default `Unavailability`.
    unavailability = createUnavailability(Clock::now());
  }

  slave::Flags CreateSlaveFlags() override
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
};


// Posts valid and invalid schedules to the maintenance schedule endpoint.
TEST_F(MasterMaintenanceTest, UpdateSchedule)
{
  // Set up a master.
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Extra machine used in this test.
  // It isn't filled in, so it's incorrect.
  MachineID badMachine;

  // Post a valid schedule with one machine.
  maintenance::Schedule schedule = createSchedule(
      {createWindow({machine1}, unavailability)});

  Future<Response> response = process::http::post(
      master.get()->pid,
      "maintenance/schedule",
      headers,
      stringify(JSON::protobuf(schedule)));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Get the maintenance schedule.
  response = process::http::get(
      master.get()->pid,
      "maintenance/schedule",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Check that the schedule was saved.
  Try<JSON::Object> masterSchedule_ =
    JSON::parse<JSON::Object>(response->body);

  ASSERT_SOME(masterSchedule_);
  Try<mesos::maintenance::Schedule> masterSchedule =
    ::protobuf::parse<mesos::maintenance::Schedule>(masterSchedule_.get());

  ASSERT_SOME(masterSchedule);
  ASSERT_EQ(1, masterSchedule->windows().size());
  ASSERT_EQ(1, masterSchedule->windows(0).machine_ids().size());
  ASSERT_EQ(
      "Machine1",
      masterSchedule->windows(0).machine_ids(0).hostname());

  // Try to replace with an invalid schedule with an empty window.
  schedule = createSchedule(
      {createWindow({}, unavailability)});

  response = process::http::post(
      master.get()->pid,
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
      master.get()->pid,
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
      master.get()->pid,
      "maintenance/schedule",
      headers,
      stringify(JSON::protobuf(schedule)));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);

  // Try to replace with another invalid schedule with duplicate machines.
  schedule = createSchedule({
      createWindow({machine1}, unavailability),
      createWindow({machine1}, unavailability)});

  response = process::http::post(
      master.get()->pid,
      "maintenance/schedule",
      headers,
      stringify(JSON::protobuf(schedule)));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);

  // Try to replace with an invalid schedule with a badly formed MachineInfo.
  schedule = createSchedule(
      {createWindow({badMachine}, unavailability)});

  response = process::http::post(
      master.get()->pid,
      "maintenance/schedule",
      headers,
      stringify(JSON::protobuf(schedule)));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);

  // Post a valid schedule with two machines.
  schedule = createSchedule(
      {createWindow({machine1, machine2}, unavailability)});

  response = process::http::post(
      master.get()->pid,
      "maintenance/schedule",
      headers,
      stringify(JSON::protobuf(schedule)));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Delete the schedule (via an empty schedule).
  schedule = createSchedule({});
  response = process::http::post(
      master.get()->pid,
      "maintenance/schedule",
      headers,
      stringify(JSON::protobuf(schedule)));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
}


// Tries to remove deactivated machines from the schedule.
TEST_F(MasterMaintenanceTest, FailToUnscheduleDeactivatedMachines)
{
  // Set up a master.
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Schedule two machines.
  maintenance::Schedule schedule = createSchedule(
      {createWindow({machine1, machine2}, unavailability)});

  Future<Response> response = process::http::post(
      master.get()->pid,
      "maintenance/schedule",
      headers,
      stringify(JSON::protobuf(schedule)));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Deactivate machine1.
  JSON::Array machines = createMachineList({machine1});
  response = process::http::post(
      master.get()->pid,
      "machine/down",
      headers,
      stringify(machines));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Try to unschedule machine1, by posting a schedule without it.
  schedule = createSchedule({createWindow({machine2}, unavailability)});

  response = process::http::post(
      master.get()->pid,
      "maintenance/schedule",
      headers,
      stringify(JSON::protobuf(schedule)));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);

  // Reactivate machine1.
  machines = createMachineList({machine1});
  response = process::http::post(
      master.get()->pid,
      "machine/up",
      headers,
      stringify(machines));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
}


// Test ensures that an offer will have an `unavailability` set if the
// slave is scheduled to go down for maintenance.
TEST_F(MasterMaintenanceTest, PendingUnavailabilityTest)
{
  master::Flags flags = CreateMasterFlags();

  Try<Owned<cluster::Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected))
    .WillRepeatedly(Return()); // Ignore future invocations.

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(connected);

  Future<Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  Future<Event::Offers> normalOffers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&normalOffers));

  {
    Call call;
    call.set_type(Call::SUBSCRIBE);

    Call::Subscribe* subscribe = call.mutable_subscribe();
    subscribe->mutable_framework_info()->CopyFrom(v1::DEFAULT_FRAMEWORK_INFO);

    mesos.send(call);
  }

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId(subscribed->framework_id());

  AWAIT_READY(normalOffers);
  EXPECT_EQ(1, normalOffers->offers().size());

  // Regular offers shouldn't have unavailability.
  EXPECT_FALSE(normalOffers->offers(0).has_unavailability());

  // The original offers should be rescinded when the unavailability is changed.
  Future<Nothing> offerRescinded;
  EXPECT_CALL(*scheduler, rescind(_, _))
    .WillOnce(FutureSatisfy(&offerRescinded));

  Future<Event::Offers> unavailabilityOffers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&unavailabilityOffers));

  Future<Event::InverseOffers> inverseOffers;
  EXPECT_CALL(*scheduler, inverseOffers(_, _))
    .WillOnce(FutureArg<1>(&inverseOffers));

  // Schedule this slave for maintenance.
  MachineID machine;
  machine.set_hostname(maintenanceHostname);
  machine.set_ip(stringify(slave.get()->pid.address.ip));

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
      master.get()->pid,
      "maintenance/schedule",
      headers,
      stringify(JSON::protobuf(schedule)));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // The original offers should be rescinded when the unavailability
  // is changed.
  AWAIT_READY(offerRescinded);

  AWAIT_READY(unavailabilityOffers);
  EXPECT_EQ(1, unavailabilityOffers->offers().size());

  // Make sure the new offers have the unavailability set.
  const v1::Offer& offer = unavailabilityOffers->offers(0);
  EXPECT_TRUE(offer.has_unavailability());
  EXPECT_EQ(
      unavailability.start().nanoseconds(),
      offer.unavailability().start().nanoseconds());

  EXPECT_EQ(
      unavailability.duration().nanoseconds(),
      offer.unavailability().duration().nanoseconds());

  // We also expect an inverse offer for the slave to go under
  // maintenance.
  AWAIT_READY(inverseOffers);
  EXPECT_EQ(1, inverseOffers->inverse_offers().size());

  // Flush any possible remaining events from the master.
  // The mocked scheduler will fail if any additional offers are sent.
  Clock::pause();
  Clock::settle();
  Clock::advance(flags.allocation_interval);
  Clock::resume();

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  EXPECT_CALL(*scheduler, disconnected(_))
    .Times(AtMost(1));
}


// Test ensures that old schedulers gracefully handle inverse offers, even if
// they aren't passed up to the top level API yet.
TEST_F(MasterMaintenanceTest, PreV1SchedulerSupport)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

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
  EXPECT_FALSE(normalOffers->empty());

  // Check that unavailability is not set.
  foreach (const Offer& offer, normalOffers.get()) {
    EXPECT_FALSE(offer.has_unavailability());
  }

  // Schedule this slave for maintenance.
  MachineID machine;
  machine.set_hostname(maintenanceHostname);
  machine.set_ip(stringify(slave.get()->pid.address.ip));

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
        master.get()->pid,
        "maintenance/schedule",
        headers,
        stringify(JSON::protobuf(schedule)));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Wait for some offers.
  AWAIT_READY(unavailabilityOffers);
  EXPECT_FALSE(unavailabilityOffers->empty());

  // Check that each offer has an unavailability.
  foreach (const Offer& offer, unavailabilityOffers.get()) {
    EXPECT_TRUE(offer.has_unavailability());
    EXPECT_EQ(unavailability.start(), offer.unavailability().start());
    EXPECT_EQ(unavailability.duration(), offer.unavailability().duration());
  }

  driver.stop();
  driver.join();
}


// Test ensures that slaves receive a shutdown message from the master when
// maintenance is started, and frameworks receive a task lost message.
TEST_F(MasterMaintenanceTest, EnterMaintenanceMode)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  slave::Flags slaveFlags = CreateSlaveFlags();

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &containerizer, slaveFlags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  // Launch a task.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, 1, 64, "*"))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  EXPECT_CALL(exec, registered(_, _, _, _));

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
  EXPECT_EQ(TASK_RUNNING, startStatus->state());

  // Schedule this slave for maintenance.
  MachineID machine;
  machine.set_hostname(maintenanceHostname);
  machine.set_ip(stringify(slave.get()->pid.address.ip));

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
        master.get()->pid,
        "maintenance/schedule",
        headers,
        stringify(JSON::protobuf(schedule)));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Verify that the master forces the slave to be shut down after the
  // maintenance is started.
  Future<ShutdownMessage> shutdownMessage =
    FUTURE_PROTOBUF(ShutdownMessage(), master.get()->pid, slave.get()->pid);

  // Verify that the framework will be informed that the slave is lost.
  Future<Nothing> slaveLost;
  EXPECT_CALL(sched, slaveLost(&driver, _))
    .WillOnce(FutureSatisfy(&slaveLost));

  // Start the maintenance.
  response =
    process::http::post(
        master.get()->pid,
        "machine/down",
        headers,
        stringify(createMachineList({machine})));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Wait for the slave to be told to shut down.
  AWAIT_READY(shutdownMessage);

  // Verify that we received a TASK_LOST.
  AWAIT_READY(lostStatus);

  EXPECT_EQ(TASK_LOST, lostStatus->state());
  EXPECT_EQ(TaskStatus::SOURCE_MASTER, lostStatus->source());
  EXPECT_EQ(TaskStatus::REASON_SLAVE_REMOVED, lostStatus->reason());

  // Verify that the framework received the slave lost message.
  AWAIT_READY(slaveLost);

  Clock::pause();
  Clock::settle();
  Clock::advance(slaveFlags.executor_shutdown_grace_period);
  Clock::resume();

  // Wait on the agent to terminate so that it wipes out its latest symlink.
  // This way when we launch a new agent it will register with a new agent id.
  wait(slave.get()->pid);

  // Ensure that the slave gets shut down immediately if it tries to register
  // from a machine that is under maintenance.
  shutdownMessage = FUTURE_PROTOBUF(ShutdownMessage(), master.get()->pid, _);
  EXPECT_TRUE(shutdownMessage.isPending());

  slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  AWAIT_READY(shutdownMessage);

  // Wait on the agent to terminate so that it wipes out its latest symlink.
  // This way when we launch a new agent it will register with a new agent id.
  wait(slave.get()->pid);

  // Stop maintenance.
  response =
    process::http::post(
        master.get()->pid,
        "machine/up",
        headers,
        stringify(createMachineList({machine})));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Capture the registration message.
  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  // Start the agent again.
  slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  // Wait for agent registration.
  AWAIT_READY(slaveRegisteredMessage);

  driver.stop();
  driver.join();
}


// Posts valid and invalid machines to the maintenance start endpoint.
TEST_F(MasterMaintenanceTest, BringDownMachines)
{
  // Set up a master.
  master::Flags flags = CreateMasterFlags();

  {
    // Default principal 2 is not allowed to start maintenance in any machine.
    mesos::ACL::StartMaintenance* acl = flags.acls->add_start_maintenances();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL_2.principal());
    acl->mutable_machines()->set_type(mesos::ACL::Entity::NONE);
  }

  // Set up a master.
  Try<Owned<cluster::Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  // Extra machine used in this test.
  // It isn't filled in, so it's incorrect.
  MachineID badMachine;

  // Try to start maintenance on an unscheduled machine.
  JSON::Array machines = createMachineList({machine1, machine2});
  Future<Response> response = process::http::post(
      master.get()->pid,
      "machine/down",
      headers,
      stringify(machines));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);

  // Try an empty list.
  machines = createMachineList({});
  response = process::http::post(
      master.get()->pid,
      "machine/down",
      headers,
      stringify(machines));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);

  // Try an empty machine.
  machines = createMachineList({badMachine});
  response = process::http::post(
      master.get()->pid,
      "machine/down",
      headers,
      stringify(machines));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);

  // Post a valid schedule with two machines.
  maintenance::Schedule schedule = createSchedule(
      {createWindow({machine1, machine2}, unavailability)});

  response = process::http::post(
      master.get()->pid,
      "maintenance/schedule",
      headers,
      stringify(JSON::protobuf(schedule)));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  process::http::Headers headersFailure =
    createBasicAuthHeaders(DEFAULT_CREDENTIAL_2);
  headersFailure["Content-Type"] = "application/json";

  // Down machine1.
  machines = createMachineList({machine1});

  response = process::http::post(
      master.get()->pid,
      "machine/down",
      headersFailure,
      stringify(machines));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Forbidden().status, response);

  response = process::http::post(
      master.get()->pid,
      "machine/down",
      headers,
      stringify(machines));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Fail to down machine1 again.
  response = process::http::post(
      master.get()->pid,
      "machine/down",
      headers,
      stringify(machines));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);

  // Fail to down machine1 and machine2.
  machines = createMachineList({machine1, machine2});
  response = process::http::post(
      master.get()->pid,
      "machine/down",
      headers,
      stringify(machines));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);

  // Down machine2.
  machines = createMachineList({machine2});
  response = process::http::post(
      master.get()->pid,
      "machine/down",
      headers,
      stringify(machines));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
}


// Posts valid and invalid machines to the maintenance stop endpoint.
TEST_F(MasterMaintenanceTest, BringUpMachines)
{
  // Set up a master.
  master::Flags flags = CreateMasterFlags();

  {
    // Default principal 2 is not allowed to stop maintenance in any machine.
    mesos::ACL::StopMaintenance* acl = flags.acls->add_stop_maintenances();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL_2.principal());
    acl->mutable_machines()->set_type(mesos::ACL::Entity::NONE);
  }

  Try<Owned<cluster::Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  // Try to bring up an unscheduled machine.
  JSON::Array machines = createMachineList({machine1, machine2});
  Future<Response> response = process::http::post(
      master.get()->pid,
      "machine/up",
      headers,
      stringify(machines));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);

  // Post a valid schedule with three machines.
  maintenance::Schedule schedule = createSchedule({
      createWindow({machine1, machine2}, unavailability),
      createWindow({machine3}, unavailability)});

  response = process::http::post(
      master.get()->pid,
      "maintenance/schedule",
      headers,
      stringify(JSON::protobuf(schedule)));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Try to bring up a non-down machine.
  machines = createMachineList({machine1, machine2});
  response = process::http::post(
      master.get()->pid,
      "machine/up",
      headers,
      stringify(machines));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);

  // Down machine3.
  machines = createMachineList({machine3});
  response = process::http::post(
      master.get()->pid,
      "machine/down",
      headers,
      stringify(machines));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  process::http::Headers headersFailure =
    createBasicAuthHeaders(DEFAULT_CREDENTIAL_2);
  headersFailure["Content-Type"] = "application/json";

  response = process::http::post(
      master.get()->pid,
      "machine/up",
      headersFailure,
      stringify(machines));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Forbidden().status, response);

  // Up machine3.
  response = process::http::post(
      master.get()->pid,
      "machine/up",
      headers,
      stringify(machines));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Get the maintenance schedule.
  response = process::http::get(
      master.get()->pid,
      "maintenance/schedule",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Check that only one maintenance window remains.
  Try<JSON::Object> masterSchedule_ =
    JSON::parse<JSON::Object>(response->body);

  ASSERT_SOME(masterSchedule_);
  Try<mesos::maintenance::Schedule> masterSchedule =
    ::protobuf::parse<mesos::maintenance::Schedule>(masterSchedule_.get());

  ASSERT_SOME(masterSchedule);
  ASSERT_EQ(1, masterSchedule->windows().size());
  ASSERT_EQ(2, masterSchedule->windows(0).machine_ids().size());

  // Down the other machines.
  machines = createMachineList({machine1, machine2});
  response = process::http::post(
      master.get()->pid,
      "machine/down",
      headers,
      stringify(machines));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Up the other machines.
  response = process::http::post(
      master.get()->pid,
      "machine/up",
      headers,
      stringify(machines));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Get the maintenance schedule again.
  response = process::http::get(
      master.get()->pid,
      "maintenance/schedule",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Check that the schedule is empty.
  masterSchedule_ = JSON::parse<JSON::Object>(response->body);

  ASSERT_SOME(masterSchedule_);
  masterSchedule =
    ::protobuf::parse<mesos::maintenance::Schedule>(masterSchedule_.get());

  ASSERT_SOME(masterSchedule);
  ASSERT_TRUE(masterSchedule->windows().empty());
}


// Queries for machine statuses in between maintenance mode transitions.
TEST_F(MasterMaintenanceTest, MachineStatus)
{
  // Set up a master.
  master::Flags flags = CreateMasterFlags();

  {
    // Default principal 2 is not allowed to view any maintenance status.
    mesos::ACL::GetMaintenanceStatus* acl =
      flags.acls->add_get_maintenance_statuses();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL_2.principal());
    acl->mutable_machines()->set_type(mesos::ACL::Entity::NONE);
  }

  // Set up a master.
  Try<Owned<cluster::Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  // Try to stop maintenance on an unscheduled machine.
  maintenance::Schedule schedule = createSchedule(
      {createWindow({machine1, machine2}, unavailability)});

  Future<Response> response = process::http::post(
      master.get()->pid,
      "maintenance/schedule",
      headers,
      stringify(JSON::protobuf(schedule)));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Get the maintenance statuses for unauthorized principal.
  response = process::http::get(
      master.get()->pid,
      "maintenance/status",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL_2));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  Try<JSON::Object> statuses_ = JSON::parse<JSON::Object>(response->body);

  ASSERT_SOME(statuses_);
  Try<maintenance::ClusterStatus> statuses =
      ::protobuf::parse<maintenance::ClusterStatus>(statuses_.get());

  ASSERT_SOME(statuses);
  ASSERT_TRUE(statuses->draining_machines().empty());
  ASSERT_TRUE(statuses->down_machines().empty());

  // Get the maintenance statuses.
  response = process::http::get(
      master.get()->pid,
      "maintenance/status",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Check that both machines are draining.
  statuses_ = JSON::parse<JSON::Object>(response->body);

  ASSERT_SOME(statuses_);
  statuses = ::protobuf::parse<maintenance::ClusterStatus>(statuses_.get());

  ASSERT_SOME(statuses);
  ASSERT_EQ(2, statuses->draining_machines().size());
  ASSERT_TRUE(statuses->down_machines().empty());

  // Deactivate machine1.
  JSON::Array machines = createMachineList({machine1});
  response = process::http::post(
      master.get()->pid,
      "machine/down",
      headers,
      stringify(machines));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Get the maintenance statuses.
  response = process::http::get(
      master.get()->pid,
      "maintenance/status",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Check one machine is deactivated.
  statuses_ = JSON::parse<JSON::Object>(response->body);

  ASSERT_SOME(statuses_);
  statuses = ::protobuf::parse<maintenance::ClusterStatus>(statuses_.get());

  ASSERT_SOME(statuses);
  ASSERT_EQ(1, statuses->draining_machines().size());
  ASSERT_EQ(1, statuses->down_machines().size());
  ASSERT_EQ("Machine1", statuses->down_machines(0).hostname());

  // Reactivate machine1.
  machines = createMachineList({machine1});
  response = process::http::post(
      master.get()->pid,
      "machine/up",
      headers,
      stringify(machines));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Get the maintenance statuses.
  response = process::http::get(
      master.get()->pid,
      "maintenance/status",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Check that only one machine remains.
  statuses_ = JSON::parse<JSON::Object>(response->body);

  ASSERT_SOME(statuses_);
  statuses = ::protobuf::parse<maintenance::ClusterStatus>(statuses_.get());

  ASSERT_SOME(statuses);
  ASSERT_EQ(1, statuses->draining_machines().size());
  ASSERT_TRUE(statuses->down_machines().empty());
  ASSERT_EQ("0.0.0.2", statuses->draining_machines(0).id().ip());
}


// Test ensures that accept and decline works with inverse offers.
// And that accepted/declined inverse offers will be reflected
// in the maintenance status endpoint.
TEST_F(MasterMaintenanceTest, InverseOffers)
{
  master::Flags masterFlags = CreateMasterFlags();

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  // Before starting any frameworks, put the one machine into `DRAINING` mode.
  MachineID machine;
  machine.set_hostname(maintenanceHostname);
  machine.set_ip(stringify(slave.get()->pid.address.ip));

  const Time start = Clock::now() + Seconds(60);
  const Duration duration = Seconds(120);
  const Unavailability unavailability = createUnavailability(start, duration);

  maintenance::Schedule schedule = createSchedule(
      {createWindow({machine}, unavailability)});

  Future<Response> response = process::http::post(
      master.get()->pid,
      "maintenance/schedule",
      headers,
      stringify(JSON::protobuf(schedule)));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Sanity check that this machine shows up in the status endpoint
  // and there should be no inverse offer status.
  response = process::http::get(
      master.get()->pid,
      "maintenance/status",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  Try<JSON::Object> statuses_ = JSON::parse<JSON::Object>(response->body);
  ASSERT_SOME(statuses_);
  Try<maintenance::ClusterStatus> statuses =
    ::protobuf::parse<maintenance::ClusterStatus>(statuses_.get());

  ASSERT_SOME(statuses);
  ASSERT_TRUE(statuses->down_machines().empty());
  ASSERT_EQ(1, statuses->draining_machines().size());
  ASSERT_EQ(
      maintenanceHostname,
      statuses->draining_machines(0).id().hostname());
  ASSERT_TRUE(statuses->draining_machines(0).statuses().empty());

  // Now start a framework.
  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected))
    .WillRepeatedly(Return()); // Ignore future invocations.

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(connected);

  Future<Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  Future<Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers));

  Future<Event::InverseOffers> inverseOffers;
  EXPECT_CALL(*scheduler, inverseOffers(_, _))
    .WillOnce(FutureArg<1>(&inverseOffers));

  {
    Call call;
    call.set_type(Call::SUBSCRIBE);

    Call::Subscribe* subscribe = call.mutable_subscribe();
    subscribe->mutable_framework_info()->CopyFrom(v1::DEFAULT_FRAMEWORK_INFO);

    mesos.send(call);
  }

  AWAIT_READY(subscribed);

  v1::FrameworkID id(subscribed->framework_id());

  // Ensure we receive some regular resource offers.
  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  // All the offers should have unavailability.
  foreach (const v1::Offer& offer, offers->offers()) {
    EXPECT_TRUE(offer.has_unavailability());
  }

  // Just work with a single offer to simplify the rest of the test.
  v1::Offer offer = offers->offers(0);

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<Event::Update> update;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&update));

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
  AWAIT_READY(inverseOffers);
  ASSERT_EQ(1, inverseOffers->inverse_offers().size());

  // Save this inverse offer so we can decline it later.
  v1::InverseOffer inverseOffer = inverseOffers->inverse_offers(0);

  // Wait for the task to start running.
  AWAIT_READY(update);
  EXPECT_EQ(v1::TASK_RUNNING, update->status().state());

  {
    // Acknowledge TASK_RUNNING update.
    Call call;
    call.mutable_framework_id()->CopyFrom(id);
    call.set_type(Call::ACKNOWLEDGE);

    Call::Acknowledge* acknowledge = call.mutable_acknowledge();
    acknowledge->mutable_task_id()->CopyFrom(taskInfo.task_id());
    acknowledge->mutable_agent_id()->CopyFrom(offer.agent_id());
    acknowledge->set_uuid(update->status().uuid());

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
    call.set_type(Call::DECLINE_INVERSE_OFFERS);

    Call::DeclineInverseOffers* decline = call.mutable_decline_inverse_offers();
    decline->add_inverse_offer_ids()->CopyFrom(inverseOffer.id());

    // Set a 0 second filter to immediately get another inverse offer.
    v1::Filters filters;
    filters.set_refuse_seconds(0);
    decline->mutable_filters()->CopyFrom(filters);

    mesos.send(call);
  }

  AWAIT_READY(updateInverseOffer);

  EXPECT_CALL(*scheduler, inverseOffers(_, _))
    .WillOnce(FutureArg<1>(&inverseOffers));

  Clock::pause();
  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  // Expect another inverse offer.
  AWAIT_READY(inverseOffers);
  Clock::resume();

  ASSERT_EQ(1, inverseOffers->inverse_offers().size());
  inverseOffer = inverseOffers->inverse_offers(0);

  // Check that the status endpoint shows the inverse offer as declined.
  response = process::http::get(
      master.get()->pid,
      "maintenance/status",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  statuses_ = JSON::parse<JSON::Object>(response->body);
  ASSERT_SOME(statuses_);
  statuses = ::protobuf::parse<maintenance::ClusterStatus>(statuses_.get());

  ASSERT_SOME(statuses);
  ASSERT_TRUE(statuses->down_machines().empty());
  ASSERT_EQ(1, statuses->draining_machines().size());
  ASSERT_EQ(
      maintenanceHostname,
      statuses->draining_machines(0).id().hostname());

  ASSERT_EQ(1, statuses->draining_machines(0).statuses().size());
  ASSERT_EQ(
      mesos::allocator::InverseOfferStatus::DECLINE,
      statuses->draining_machines(0).statuses(0).status());

  ASSERT_EQ(
      id,
      evolve(statuses->draining_machines(0).statuses(0).framework_id()));

  updateInverseOffer =
    FUTURE_DISPATCH(_, &MesosAllocatorProcess::updateInverseOffer);

  {
    // Accept an inverse offer, with filter.
    Call call;
    call.mutable_framework_id()->CopyFrom(id);
    call.set_type(Call::ACCEPT_INVERSE_OFFERS);

    Call::AcceptInverseOffers* accept = call.mutable_accept_inverse_offers();
    accept->add_inverse_offer_ids()->CopyFrom(inverseOffer.id());

    // Set a 0 second filter to immediately get another inverse offer.
    v1::Filters filters;
    filters.set_refuse_seconds(0);
    accept->mutable_filters()->CopyFrom(filters);

    mesos.send(call);
  }

  AWAIT_READY(updateInverseOffer);

  EXPECT_CALL(*scheduler, inverseOffers(_, _))
    .WillOnce(FutureArg<1>(&inverseOffers));

  Clock::pause();
  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  // Expect yet another inverse offer.
  AWAIT_READY(inverseOffers);
  Clock::resume();

  ASSERT_EQ(1, inverseOffers->inverse_offers().size());

  // Check that the status endpoint shows the inverse offer as accepted.
  response = process::http::get(
      master.get()->pid,
      "maintenance/status",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  statuses_ = JSON::parse<JSON::Object>(response->body);
  ASSERT_SOME(statuses_);
  statuses = ::protobuf::parse<maintenance::ClusterStatus>(statuses_.get());

  ASSERT_SOME(statuses);
  ASSERT_TRUE(statuses->down_machines().empty());
  ASSERT_EQ(1, statuses->draining_machines().size());
  ASSERT_EQ(
      maintenanceHostname,
      statuses->draining_machines(0).id().hostname());

  ASSERT_EQ(1, statuses->draining_machines(0).statuses().size());
  ASSERT_EQ(
      mesos::allocator::InverseOfferStatus::ACCEPT,
      statuses->draining_machines(0).statuses(0).status());

  ASSERT_EQ(
      id,
      evolve(statuses->draining_machines(0).statuses(0).framework_id()));

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));
}


// Test ensures that inverse offers support filters.
TEST_F(MasterMaintenanceTest, InverseOffersFilters)
{
  master::Flags flags = CreateMasterFlags();

  Try<Owned<cluster::Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  ExecutorInfo executor1 = createExecutorInfo("executor-1", "exit 1");
  ExecutorInfo executor2 = createExecutorInfo("executor-2", "exit 2");

  MockExecutor exec1(executor1.executor_id());
  MockExecutor exec2(executor2.executor_id());

  hashmap<ExecutorID, Executor*> execs;
  execs[executor1.executor_id()] = &exec1;
  execs[executor2.executor_id()] = &exec2;

  TestContainerizer containerizer(execs);

  EXPECT_CALL(exec1, registered(_, _, _, _));

  EXPECT_CALL(exec1, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  EXPECT_CALL(exec2, registered(_, _, _, _));

  EXPECT_CALL(exec2, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  // Capture the registration message for the first slave.
  Future<SlaveRegisteredMessage> slave1RegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  Owned<MasterDetector> detector = master.get()->createDetector();

  // We need two agents for this test.
  Try<Owned<cluster::Slave>> slave1 =
    StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave1);

  // We need to make sure the first slave registers before we schedule the
  // machine it is running on for maintenance.
  AWAIT_READY(slave1RegisteredMessage);

  // Capture the registration message for the second slave.
  Future<SlaveRegisteredMessage> slave2RegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, Not(slave1.get()->pid));

  slave::Flags slaveFlags2 = MesosTest::CreateSlaveFlags();
  slaveFlags2.hostname = maintenanceHostname + "-2";

  Try<Owned<cluster::Slave>> slave2 =
    StartSlave(detector.get(), &containerizer, slaveFlags2);
  ASSERT_SOME(slave2);

  // We need to make sure the second slave registers before we schedule the
  // machine it is running on for maintenance.
  AWAIT_READY(slave2RegisteredMessage);

  // Before starting any frameworks, put the first machine into `DRAINING` mode.
  MachineID machine1;
  machine1.set_hostname(maintenanceHostname);
  machine1.set_ip(stringify(slave1.get()->pid.address.ip));

  MachineID machine2;
  machine2.set_hostname(slaveFlags2.hostname.get());
  machine2.set_ip(stringify(slave2.get()->pid.address.ip));

  const Time start = Clock::now() + Seconds(60);
  const Duration duration = Seconds(120);
  const Unavailability unavailability = createUnavailability(start, duration);

  maintenance::Schedule schedule = createSchedule(
      {createWindow({machine1, machine2}, unavailability)});

  Future<Response> response = process::http::post(
      master.get()->pid,
      "maintenance/schedule",
      headers,
      stringify(JSON::protobuf(schedule)));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  // Now start a framework.
  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected))
    .WillRepeatedly(Return()); // Ignore future invocations.

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(connected);

  Future<Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  Future<Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers));

  Future<Event::InverseOffers> inverseOffers;
  EXPECT_CALL(*scheduler, inverseOffers(_, _))
    .WillOnce(FutureArg<1>(&inverseOffers));

  // Pause the clock before subscribing the framework.
  // This ensures deterministic offer-ing behavior during the test.
  Clock::pause();

  {
    Call call;
    call.set_type(Call::SUBSCRIBE);

    Call::Subscribe* subscribe = call.mutable_subscribe();
    subscribe->mutable_framework_info()->CopyFrom(v1::DEFAULT_FRAMEWORK_INFO);

    mesos.send(call);
  }

  AWAIT_READY(subscribed);

  v1::FrameworkID id(subscribed->framework_id());

  // Trigger a batch allocation.
  Clock::advance(flags.allocation_interval);

  AWAIT_READY(offers);
  EXPECT_EQ(2, offers->offers().size());

  // All the offers should have unavailability.
  foreach (const v1::Offer& offer, offers->offers()) {
    EXPECT_TRUE(offer.has_unavailability());
  }

  // Save both offers.
  v1::Offer offer1 = offers->offers(0);
  v1::Offer offer2 = offers->offers(1);

  // Spawn dummy tasks using both offers.
  v1::TaskInfo taskInfo1 =
    evolve(createTask(devolve(offer1), "exit 1", executor1.executor_id()));

  v1::TaskInfo taskInfo2 =
    evolve(createTask(devolve(offer2), "exit 2", executor2.executor_id()));

  Future<Event::Update> update1;
  Future<Event::Update> update2;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&update1))
    .WillOnce(FutureArg<1>(&update2));

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
  AWAIT_READY(inverseOffers);
  ASSERT_EQ(2, inverseOffers->inverse_offers().size());

  // Save these inverse offers.
  v1::InverseOffer inverseOffer1 = inverseOffers->inverse_offers(0);
  v1::InverseOffer inverseOffer2 = inverseOffers->inverse_offers(1);

  // We want to acknowledge TASK_RUNNING updates for the two tasks we
  // have launched. We don't know which task will be launched first,
  // so just send acknowledgments in response to the TASK_RUNNING
  // events we receive. We track which task ids we acknowledge, and
  // then verify them with the expected task ids.
  hashset<v1::TaskID> acknowledgedTaskIds;

  AWAIT_READY(update1);
  EXPECT_EQ(v1::TASK_RUNNING, update1->status().state());

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(id);
    call.set_type(Call::ACKNOWLEDGE);

    Call::Acknowledge* acknowledge = call.mutable_acknowledge();
    acknowledge->mutable_task_id()->CopyFrom(update1->status().task_id());
    acknowledge->mutable_agent_id()->CopyFrom(update1->status().agent_id());
    acknowledge->set_uuid(update1->status().uuid());

    EXPECT_FALSE(acknowledgedTaskIds.contains(update1->status().task_id()));
    acknowledgedTaskIds.insert(update1->status().task_id());

    mesos.send(call);
  }

  AWAIT_READY(update2);
  EXPECT_EQ(v1::TASK_RUNNING, update2->status().state());

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(id);
    call.set_type(Call::ACKNOWLEDGE);

    Call::Acknowledge* acknowledge = call.mutable_acknowledge();
    acknowledge->mutable_task_id()->CopyFrom(update2->status().task_id());
    acknowledge->mutable_agent_id()->CopyFrom(update2->status().agent_id());
    acknowledge->set_uuid(update2->status().uuid());

    EXPECT_FALSE(acknowledgedTaskIds.contains(update2->status().task_id()));
    acknowledgedTaskIds.insert(update2->status().task_id());

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
    call.set_type(Call::DECLINE_INVERSE_OFFERS);

    Call::DeclineInverseOffers* decline = call.mutable_decline_inverse_offers();
    decline->add_inverse_offer_ids()->CopyFrom(inverseOffer2.id());

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
    call.set_type(Call::ACCEPT_INVERSE_OFFERS);

    Call::AcceptInverseOffers* accept = call.mutable_accept_inverse_offers();
    accept->add_inverse_offer_ids()->CopyFrom(inverseOffer1.id());

    v1::Filters filters;
    filters.set_refuse_seconds(0);
    accept->mutable_filters()->CopyFrom(filters);

    mesos.send(call);
  }

  AWAIT_READY(updateInverseOffer);

  EXPECT_CALL(*scheduler, inverseOffers(_, _))
    .WillOnce(FutureArg<1>(&inverseOffers));

  Clock::settle();
  Clock::advance(flags.allocation_interval);

  // Expect one inverse offer in this batch allocation.
  AWAIT_READY(inverseOffers);

  ASSERT_EQ(1, inverseOffers->inverse_offers().size());
  EXPECT_EQ(
      inverseOffer1.agent_id(),
      inverseOffers->inverse_offers(0).agent_id());

  inverseOffer1 = inverseOffers->inverse_offers(0);

  updateInverseOffer =
    FUTURE_DISPATCH(_, &MesosAllocatorProcess::updateInverseOffer);

  {
    // Do another immediate filter, but decline it this time.
    Call call;
    call.mutable_framework_id()->CopyFrom(id);
    call.set_type(Call::DECLINE_INVERSE_OFFERS);

    Call::DeclineInverseOffers* decline = call.mutable_decline_inverse_offers();
    decline->add_inverse_offer_ids()->CopyFrom(inverseOffer1.id());

    v1::Filters filters;
    filters.set_refuse_seconds(0);
    decline->mutable_filters()->CopyFrom(filters);

    mesos.send(call);
  }

  AWAIT_READY(updateInverseOffer);

  EXPECT_CALL(*scheduler, inverseOffers(_, _))
    .WillOnce(FutureArg<1>(&inverseOffers));

  Clock::settle();
  Clock::advance(flags.allocation_interval);

  // Expect the same inverse offer in this batch allocation.
  AWAIT_READY(inverseOffers);

  ASSERT_EQ(1, inverseOffers->inverse_offers().size());
  EXPECT_EQ(
      inverseOffer1.agent_id(),
      inverseOffers->inverse_offers(0).agent_id());

  EXPECT_CALL(exec1, shutdown(_))
    .Times(AtMost(1));

  EXPECT_CALL(exec2, shutdown(_))
    .Times(AtMost(1));
}


// Testing post and get without authentication and with bad credentials.
TEST_F(MasterMaintenanceTest, EndpointsBadAuthentication)
{
  // Set up a master with authentication required.
  // Note that the default master test flags enable HTTP authentication.
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Headers for POSTs to maintenance endpoints without authentication.
  process::http::Headers unauthenticatedHeaders;
  unauthenticatedHeaders["Content-Type"] = "application/json";

  // A valid schedule with one machine.
  maintenance::Schedule schedule = createSchedule(
      {createWindow({machine1}, unavailability)});

  // Bad credentials which should fail authentication.
  Credential badCredential;
  badCredential.set_principal("badPrincipal");
  badCredential.set_secret("badSecret");

  // Headers for POSTs to maintenance endpoints with bad authentication.
  process::http::Headers badAuthenticationHeaders;
  badAuthenticationHeaders = createBasicAuthHeaders(badCredential);
  badAuthenticationHeaders["Content-Type"] = "application/json";

  // maintenance/schedule endpoint.
  {
    // Post the maintenance schedule without authentication.
    Future<Response> response = process::http::post(
        master.get()->pid,
        "maintenance/schedule",
        unauthenticatedHeaders,
        stringify(JSON::protobuf(schedule)));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response);

    // Get the maintenance schedule without authentication.
    response = process::http::get(master.get()->pid, "maintenance/schedule");

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response);

    // Post the maintenance schedule with bad authentication.
    response = process::http::post(
        master.get()->pid,
        "maintenance/schedule",
        badAuthenticationHeaders,
        stringify(JSON::protobuf(schedule)));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response);

    // Get the maintenance schedule with bad authentication.
    response = process::http::get(
        master.get()->pid,
        "maintenance/schedule",
        None(),
        createBasicAuthHeaders(badCredential));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response);
  }

  // machine/up endpoint.
  {
    // Post to machine/up without authentication.
    Future<Response> response = process::http::post(
        master.get()->pid,
        "machine/up",
        unauthenticatedHeaders,
        stringify(JSON::protobuf(schedule)));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response);

    // Post to machine/up with bad authentication.
    response = process::http::post(
        master.get()->pid,
        "machine/up",
        badAuthenticationHeaders,
        stringify(JSON::protobuf(schedule)));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response);
  }

  // machine/down endpoint.
  {
    // Post to machine/down without authentication.
    Future<Response> response = process::http::post(
        master.get()->pid,
        "machine/down",
        unauthenticatedHeaders,
        stringify(JSON::protobuf(schedule)));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response);

    // Post to machine/down with bad authentication.
    response = process::http::post(
        master.get()->pid,
        "machine/down",
        badAuthenticationHeaders,
        stringify(JSON::protobuf(schedule)));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response);
  }

  // maintenance/status endpoint.
  {
    // Get the maintenance status without authentication.
    Future<Response> response = process::http::get(
        master.get()->pid,
        "maintenance/status");

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response);

    // Get the maintenance status with bad authentication.
    response = process::http::get(
        master.get()->pid,
        "maintenance/status",
        None(),
        createBasicAuthHeaders(badCredential));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response);
  }
}


// This test verifies that the Mesos master does not crash while
// processing an invalid inverse offer (See MESOS-7119).
TEST_F(MasterMaintenanceTest, AcceptInvalidInverseOffer)
{
  master::Flags masterFlags = CreateMasterFlags();

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<Message> frameworkRegisteredMessage =
    FUTURE_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()), _, _);

  driver.start();

  AWAIT_READY(frameworkRegisteredMessage);
  UPID frameworkPid = frameworkRegisteredMessage->to;

  FrameworkRegisteredMessage message;
  ASSERT_TRUE(message.ParseFromString(frameworkRegisteredMessage->body));

  FrameworkID frameworkId = message.framework_id();

  Future<mesos::scheduler::Call> acceptInverseOffersCall = FUTURE_CALL(
      mesos::scheduler::Call(),
      mesos::scheduler::Call::ACCEPT_INVERSE_OFFERS,
      _,
      _);

  {
    mesos::scheduler::Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(mesos::scheduler::Call::ACCEPT_INVERSE_OFFERS);

    mesos::scheduler::Call::AcceptInverseOffers* accept =
      call.mutable_accept_inverse_offers();

    accept->add_inverse_offer_ids()->set_value("invalid-inverse-offer");

    process::post(frameworkPid, master.get()->pid, call);
  }

  AWAIT_READY(acceptInverseOffersCall);

  driver.stop();
  driver.join();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
