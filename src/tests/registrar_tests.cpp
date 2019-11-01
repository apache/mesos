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

#include <algorithm>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <google/protobuf/util/message_differencer.h>

#include <mesos/attributes.hpp>
#include <mesos/type_utils.hpp>

#include <mesos/authentication/http/basic_authenticator_factory.hpp>

#include <mesos/log/log.hpp>

#include <mesos/state/log.hpp>
#include <mesos/state/state.hpp>
#include <mesos/state/storage.hpp>

#include <process/clock.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/pid.hpp>
#include <process/process.hpp>

#include <stout/bytes.hpp>
#include <stout/stopwatch.hpp>
#include <stout/uuid.hpp>

#include <stout/tests/utils.hpp>

#include "common/protobuf_utils.hpp"

#include "log/replica.hpp"

#include "log/tool/initialize.hpp"

#include "master/flags.hpp"
#include "master/maintenance.hpp"
#include "master/master.hpp"
#include "master/quota.hpp"
#include "master/registrar.hpp"
#include "master/registry_operations.hpp"
#include "master/weights.hpp"

#include "tests/mesos.hpp"

using namespace mesos::internal::master;

using namespace process;

using mesos::log::Log;

using mesos::internal::log::Replica;

using std::cout;
using std::endl;
using std::map;
using std::pair;
using std::set;
using std::string;
using std::vector;

using process::Clock;
using process::Owned;

using process::http::OK;
using process::http::Response;
using process::http::Unauthorized;

using google::protobuf::Map;
using google::protobuf::RepeatedPtrField;
using google::protobuf::util::MessageDifferencer;

using mesos::internal::protobuf::maintenance::createMachineList;
using mesos::internal::protobuf::maintenance::createSchedule;
using mesos::internal::protobuf::maintenance::createUnavailability;
using mesos::internal::protobuf::maintenance::createWindow;

using testing::_;
using testing::DoAll;
using testing::Eq;
using testing::Return;

using ::testing::WithParamInterface;

namespace mesos {
namespace internal {
namespace tests {

namespace quota = mesos::internal::master::quota;

namespace authentication = process::http::authentication;

using namespace mesos::maintenance;
using namespace mesos::quota;

using namespace mesos::internal::master::maintenance;
using namespace mesos::internal::master::quota;
using namespace mesos::internal::master::weights;

using mesos::http::authentication::BasicAuthenticatorFactory;

using mesos::state::LogStorage;
using mesos::state::State;
using mesos::state::Storage;

using state::Entry;


static vector<WeightInfo> getWeightInfos(
    const hashmap<string, double>& weights) {
  vector<WeightInfo> weightInfos;

  foreachpair (const string& role, double weight, weights) {
    WeightInfo weightInfo;
    weightInfo.set_role(role);
    weightInfo.set_weight(weight);
    weightInfos.push_back(weightInfo);
  }

  return weightInfos;
}


// TODO(xujyan): This class copies code from LogStateTest. It would
// be nice to find a common location for log related base tests when
// there are more uses of it.
class RegistrarTestBase : public TemporaryDirectoryTest
{
public:
  RegistrarTestBase()
    : log(nullptr),
      storage(nullptr),
      state(nullptr),
      replica2(nullptr) {}

protected:
  void SetUp() override
  {
    TemporaryDirectoryTest::SetUp();

    // For initializing the replicas.
    log::tool::Initialize initializer;

    string path1 = os::getcwd() + "/.log1";
    string path2 = os::getcwd() + "/.log2";

    initializer.flags.path = path1;
    initializer.execute();

    initializer.flags.path = path2;
    initializer.execute();

    // Only create the replica for 'path2' (i.e., the second replica)
    // as the first replica will be created when we create a Log.
    replica2 = new Replica(path2);

    set<UPID> pids;
    pids.insert(replica2->pid());

    log = new Log(2, path1, pids);
    storage = new LogStorage(log);
    state = new State(storage);

    // Compensate for slow CI machines / VMs.
    flags.registry_store_timeout = process::TEST_AWAIT_TIMEOUT;

    master.CopyFrom(protobuf::createMasterInfo(UPID("master@127.0.0.1:5050")));

    SlaveID id;
    id.set_value("1");

    SlaveInfo info;
    info.set_hostname("localhost");
    info.mutable_id()->CopyFrom(id);

    slave.CopyFrom(info);
  }

  void TearDown() override
  {
    delete state;
    delete storage;
    delete log;
    delete replica2;

    TemporaryDirectoryTest::TearDown();
  }

  Log* log;
  Storage* storage;
  State* state;

  Replica* replica2;

  MasterInfo master;
  SlaveInfo slave;
  Flags flags;
};


class RegistrarTest : public RegistrarTestBase {};


TEST_F(RegistrarTest, Recover)
{
  Registrar registrar(flags, state);

  // Operations preceding recovery will fail.
  AWAIT_EXPECT_FAILED(registrar.apply(Owned<RegistryOperation>(
      new AdmitSlave(slave))));
  AWAIT_EXPECT_FAILED(registrar.apply(Owned<RegistryOperation>(
      new MarkSlaveUnreachable(slave, protobuf::getCurrentTime()))));
  AWAIT_EXPECT_FAILED(registrar.apply(Owned<RegistryOperation>(
      new MarkSlaveReachable(slave))));
  AWAIT_EXPECT_FAILED(registrar.apply(Owned<RegistryOperation>(
      new RemoveSlave(slave))));

  Future<Registry> registry = registrar.recover(master);

  // Before waiting for the recovery to complete, invoke some
  // operations to ensure they do not fail.
  Future<bool> admit = registrar.apply(Owned<RegistryOperation>(
      new AdmitSlave(slave)));
  Future<bool> unreachable = registrar.apply(Owned<RegistryOperation>(
      new MarkSlaveUnreachable(slave, protobuf::getCurrentTime())));
  Future<bool> reachable = registrar.apply(Owned<RegistryOperation>(
      new MarkSlaveReachable(slave)));
  Future<bool> remove = registrar.apply(Owned<RegistryOperation>(
      new RemoveSlave(slave)));

  AWAIT_READY(registry);
  EXPECT_EQ(master, registry->master().info());

  AWAIT_TRUE(admit);
  AWAIT_TRUE(unreachable);
  AWAIT_TRUE(reachable);
  AWAIT_TRUE(remove);
}


TEST_F(RegistrarTest, Admit)
{
  Registrar registrar(flags, state);
  AWAIT_READY(registrar.recover(master));

  AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(new AdmitSlave(slave))));
  AWAIT_FALSE(registrar.apply(Owned<RegistryOperation>(new AdmitSlave(slave))));
}


TEST_F(RegistrarTest, UpdateSlave)
{
  // Add a new slave to the registry.
  {
    Registrar registrar(flags, state);
    AWAIT_READY(registrar.recover(master));

    slave.set_hostname("original");
    AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
        new AdmitSlave(slave))));
  }


  // Verify that the slave is present, and update its hostname.
  {
    Registrar registrar(flags, state);
    Future<Registry> registry = registrar.recover(master);
    AWAIT_READY(registry);

    ASSERT_EQ(1, registry->slaves().slaves().size());
    EXPECT_EQ("original", registry->slaves().slaves(0).info().hostname());

    slave.set_hostname("changed");
    AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
        new UpdateSlave(slave))));
  }

  // Verify that the hostname indeed changed, and do one additional update
  // to check that the operation is idempotent.
  {
    Registrar registrar(flags, state);
    Future<Registry> registry = registrar.recover(master);
    AWAIT_READY(registry);

    ASSERT_EQ(1, registry->slaves().slaves().size());
    EXPECT_EQ("changed", registry->slaves().slaves(0).info().hostname());

    AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
        new UpdateSlave(slave))));
  }

  // Verify that nothing changed from the second update.
  {
    Registrar registrar(flags, state);
    Future<Registry> registry = registrar.recover(master);
    AWAIT_READY(registry);

    ASSERT_EQ(1, registry->slaves().slaves().size());
    EXPECT_EQ("changed", registry->slaves().slaves(0).info().hostname());
  }
}


TEST_F(RegistrarTest, MarkReachable)
{
  Registrar registrar(flags, state);
  AWAIT_READY(registrar.recover(master));

  SlaveID id1;
  id1.set_value("1");

  SlaveInfo info1;
  info1.set_hostname("localhost");
  info1.mutable_id()->CopyFrom(id1);

  SlaveID id2;
  id2.set_value("2");

  SlaveInfo info2;
  info2.set_hostname("localhost");
  info2.mutable_id()->CopyFrom(id2);

  AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
      new AdmitSlave(info1))));

  // Attempting to mark a slave as reachable that is already reachable
  // should not result in an error.
  AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
      new MarkSlaveReachable(info1))));
  AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
      new MarkSlaveReachable(info1))));

  // Attempting to mark a slave as reachable that is not in the
  // unreachable list should not result in error.
  AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
      new MarkSlaveReachable(info2))));
}


TEST_F(RegistrarTest, MarkUnreachable)
{
  Registrar registrar(flags, state);
  AWAIT_READY(registrar.recover(master));

  SlaveID id1;
  id1.set_value("1");

  SlaveInfo info1;
  info1.set_hostname("localhost");
  info1.mutable_id()->CopyFrom(id1);

  SlaveID id2;
  id2.set_value("2");

  SlaveInfo info2;
  info2.set_hostname("localhost");
  info2.mutable_id()->CopyFrom(id2);

  AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
      new AdmitSlave(info1))));

  AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
      new MarkSlaveUnreachable(info1, protobuf::getCurrentTime()))));

  AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
      new MarkSlaveReachable(info1))));

  AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
      new MarkSlaveUnreachable(info1, protobuf::getCurrentTime()))));

  // If a slave is already unreachable, trying to mark it unreachable
  // again should fail.
  AWAIT_FALSE(registrar.apply(Owned<RegistryOperation>(
      new MarkSlaveUnreachable(info1, protobuf::getCurrentTime()))));
}


// Verify that an admitted slave can be marked as gone.
TEST_F(RegistrarTest, MarkGone)
{
  Registrar registrar(flags, state);
  AWAIT_READY(registrar.recover(master));

  SlaveID id1;
  id1.set_value("1");

  SlaveInfo info1;
  info1.set_hostname("localhost");
  info1.mutable_id()->CopyFrom(id1);

  AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
      new AdmitSlave(info1))));

  AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
      new MarkSlaveGone(info1.id(), protobuf::getCurrentTime()))));
}


// Verify that an unreachable slave can be marked as gone.
TEST_F(RegistrarTest, MarkUnreachableGone)
{
  Registrar registrar(flags, state);
  AWAIT_READY(registrar.recover(master));

  SlaveID id1;
  id1.set_value("1");

  SlaveInfo info1;
  info1.set_hostname("localhost");
  info1.mutable_id()->CopyFrom(id1);

  AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
      new AdmitSlave(info1))));

  AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
      new MarkSlaveUnreachable(info1, protobuf::getCurrentTime()))));

  AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
      new MarkSlaveGone(info1.id(), protobuf::getCurrentTime()))));

  // If a slave is already gone, trying to mark it gone again should fail.
  AWAIT_FALSE(registrar.apply(Owned<RegistryOperation>(
      new MarkSlaveGone(info1.id(), protobuf::getCurrentTime()))));

  // If a slave is already gone, trying to mark it unreachable
  // again should fail.
  AWAIT_FALSE(registrar.apply(Owned<RegistryOperation>(
      new MarkSlaveUnreachable(info1, protobuf::getCurrentTime()))));
}


TEST_F(RegistrarTest, Prune)
{
  Registrar registrar(flags, state);
  AWAIT_READY(registrar.recover(master));

  SlaveID id1;
  id1.set_value("1");

  SlaveInfo info1;
  info1.set_hostname("localhost");
  info1.mutable_id()->CopyFrom(id1);

  SlaveID id2;
  id2.set_value("2");

  SlaveInfo info2;
  info2.set_hostname("localhost");
  info2.mutable_id()->CopyFrom(id2);

  SlaveID id3;
  id3.set_value("3");

  SlaveInfo info3;
  info3.set_hostname("localhost");
  info3.mutable_id()->CopyFrom(id3);

  AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
      new AdmitSlave(info1))));
  AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
      new AdmitSlave(info2))));
  AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
      new AdmitSlave(info3))));

  AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
      new MarkSlaveUnreachable(info1, protobuf::getCurrentTime()))));

  AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
      new MarkSlaveUnreachable(info2, protobuf::getCurrentTime()))));

  AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
      new MarkSlaveGone(info3.id(), protobuf::getCurrentTime()))));

  AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
      new Prune({id1}, {}))));
  AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
      new Prune({id2}, {}))));
  AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
      new Prune({}, {id3}))));

  AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
      new AdmitSlave(info1))));
  AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
      new AdmitSlave(info2))));
  AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
      new AdmitSlave(info3))));

  AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
      new MarkSlaveUnreachable(info1, protobuf::getCurrentTime()))));

  AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
      new MarkSlaveUnreachable(info2, protobuf::getCurrentTime()))));

  AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
      new MarkSlaveGone(info3.id(), protobuf::getCurrentTime()))));

  AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
      new Prune({id1}, {}))));
  AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
      new Prune({id2}, {}))));
  AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
      new Prune({}, {id3}))));
}


TEST_F(RegistrarTest, Remove)
{
  Registrar registrar(flags, state);
  AWAIT_READY(registrar.recover(master));

  SlaveID id1;
  id1.set_value("1");

  SlaveInfo info1;
  info1.set_hostname("localhost");
  info1.mutable_id()->CopyFrom(id1);

  SlaveID id2;
  id2.set_value("2");

  SlaveInfo info2;
  info2.set_hostname("localhost");
  info2.mutable_id()->CopyFrom(id2);

  SlaveID id3;
  id3.set_value("3");

  SlaveInfo info3;
  info3.set_hostname("localhost");
  info3.mutable_id()->CopyFrom(id3);

  AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
      new AdmitSlave(info1))));
  AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
      new AdmitSlave(info2))));
  AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
      new AdmitSlave(info3))));
  AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
      new RemoveSlave(info1))));
  AWAIT_FALSE(registrar.apply(Owned<RegistryOperation>(
      new RemoveSlave(info1))));
  AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
      new AdmitSlave(info1))));
  AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
      new RemoveSlave(info2))));
  AWAIT_FALSE(registrar.apply(Owned<RegistryOperation>(
      new RemoveSlave(info2))));
  AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
      new RemoveSlave(info3))));
  AWAIT_FALSE(registrar.apply(Owned<RegistryOperation>(
      new RemoveSlave(info3))));
}


// NOTE: For the following tests, the state of the registrar can
// only be viewed once per instantiation of the registrar.
// To check the result of each operation, we must re-construct
// the registrar, which is done by putting the code into scoped blocks.

// TODO(josephw): Consider refactoring these maintenance operation tests
// to use a helper function for each un-named scoped block.
// For example:
//   MaintenanceTest(flags, state, [=](const Registry& registry) {
//     // Checks and operations.  i.e.:
//     EXPECT_EQ(1, registry->schedules().size());
//   });

// Adds maintenance schedules to the registry, one machine at a time.
// Then removes machines from the schedule.
TEST_F(RegistrarTest, UpdateMaintenanceSchedule)
{
  // Machine definitions used in this test.
  MachineID machine1;
  machine1.set_ip("0.0.0.1");

  MachineID machine2;
  machine2.set_hostname("2");

  MachineID machine3;
  machine3.set_hostname("3");
  machine3.set_ip("0.0.0.3");

  Unavailability unavailability = createUnavailability(Clock::now());

  {
    // Prepare the registrar.
    Registrar registrar(flags, state);
    AWAIT_READY(registrar.recover(master));

    // Schedule one machine for maintenance.
    maintenance::Schedule schedule = createSchedule(
        {createWindow({machine1}, unavailability)});

    AWAIT_READY(registrar.apply(Owned<RegistryOperation>(
        new UpdateSchedule(schedule))));
  }

  {
    // Check that one schedule and one machine info was made.
    Registrar registrar(flags, state);
    Future<Registry> registry = registrar.recover(master);
    AWAIT_READY(registry);

    EXPECT_EQ(1, registry->schedules().size());
    EXPECT_EQ(1, registry->schedules(0).windows().size());
    EXPECT_EQ(1, registry->schedules(0).windows(0).machine_ids().size());
    EXPECT_EQ(1, registry->machines().machines().size());
    EXPECT_EQ(
        MachineInfo::DRAINING,
        registry->machines().machines(0).info().mode());

    // Extend the schedule by one machine (in a different window).
    maintenance::Schedule schedule = createSchedule({
        createWindow({machine1}, unavailability),
        createWindow({machine2}, unavailability)});

    AWAIT_READY(registrar.apply(Owned<RegistryOperation>(
        new UpdateSchedule(schedule))));
  }

  {
    // Check that both machines are part of maintenance.
    Registrar registrar(flags, state);
    Future<Registry> registry = registrar.recover(master);
    AWAIT_READY(registry);

    EXPECT_EQ(1, registry->schedules().size());
    EXPECT_EQ(2, registry->schedules(0).windows().size());
    EXPECT_EQ(1, registry->schedules(0).windows(0).machine_ids().size());
    EXPECT_EQ(1, registry->schedules(0).windows(1).machine_ids().size());
    EXPECT_EQ(2, registry->machines().machines().size());
    EXPECT_EQ(
        MachineInfo::DRAINING,
        registry->machines().machines(1).info().mode());

    // Extend a window by one machine.
    maintenance::Schedule schedule = createSchedule({
        createWindow({machine1}, unavailability),
        createWindow({machine2, machine3}, unavailability)});

    AWAIT_READY(registrar.apply(Owned<RegistryOperation>(
        new UpdateSchedule(schedule))));
  }

  {
    // Check that all three machines are included.
    Registrar registrar(flags, state);
    Future<Registry> registry = registrar.recover(master);
    AWAIT_READY(registry);

    EXPECT_EQ(1, registry->schedules().size());
    EXPECT_EQ(2, registry->schedules(0).windows().size());
    EXPECT_EQ(1, registry->schedules(0).windows(0).machine_ids().size());
    EXPECT_EQ(2, registry->schedules(0).windows(1).machine_ids().size());
    EXPECT_EQ(3, registry->machines().machines().size());
    EXPECT_EQ(
        MachineInfo::DRAINING,
        registry->machines().machines(2).info().mode());

    // Rearrange the schedule into one window.
    maintenance::Schedule schedule = createSchedule(
        {createWindow({machine1, machine2, machine3}, unavailability)});

    AWAIT_READY(registrar.apply(Owned<RegistryOperation>(
        new UpdateSchedule(schedule))));
  }

  {
    // Check that the machine infos are unchanged, but the schedule is.
    Registrar registrar(flags, state);
    Future<Registry> registry = registrar.recover(master);
    AWAIT_READY(registry);

    EXPECT_EQ(1, registry->schedules().size());
    EXPECT_EQ(1, registry->schedules(0).windows().size());
    EXPECT_EQ(3, registry->schedules(0).windows(0).machine_ids().size());
    EXPECT_EQ(3, registry->machines().machines().size());
    EXPECT_EQ(
        MachineInfo::DRAINING,
        registry->machines().machines(0).info().mode());

    EXPECT_EQ(
        MachineInfo::DRAINING,
        registry->machines().machines(1).info().mode());

    EXPECT_EQ(
        MachineInfo::DRAINING,
        registry->machines().machines(2).info().mode());

    // Delete one machine from the schedule.
    maintenance::Schedule schedule = createSchedule(
        {createWindow({machine2, machine3}, unavailability)});

    AWAIT_READY(registrar.apply(Owned<RegistryOperation>(
        new UpdateSchedule(schedule))));
  }

  {
    // Check that one machine info is removed.
    Registrar registrar(flags, state);
    Future<Registry> registry = registrar.recover(master);
    AWAIT_READY(registry);

    EXPECT_EQ(1, registry->schedules().size());
    EXPECT_EQ(1, registry->schedules(0).windows().size());
    EXPECT_EQ(2, registry->schedules(0).windows(0).machine_ids().size());
    EXPECT_EQ(2, registry->machines().machines().size());

    // Delete all machines from the schedule.
    maintenance::Schedule schedule;
    AWAIT_READY(registrar.apply(Owned<RegistryOperation>(
        new UpdateSchedule(schedule))));
  }

  {
    // Check that all statuses are removed.
    Registrar registrar(flags, state);
    Future<Registry> registry = registrar.recover(master);
    AWAIT_READY(registry);

    EXPECT_EQ(1, registry->schedules().size());
    EXPECT_TRUE(registry->schedules(0).windows().empty());
    EXPECT_TRUE(registry->machines().machines().empty());
  }
}


// Creates a schedule and properly starts maintenance.
TEST_F(RegistrarTest, StartMaintenance)
{
  // Machine definitions used in this test.
  MachineID machine1;
  machine1.set_ip("0.0.0.1");

  MachineID machine2;
  machine2.set_hostname("2");

  MachineID machine3;
  machine3.set_hostname("3");
  machine3.set_ip("0.0.0.3");

  Unavailability unavailability = createUnavailability(Clock::now());

  {
    // Prepare the registrar.
    Registrar registrar(flags, state);
    AWAIT_READY(registrar.recover(master));

    // Schedule two machines for maintenance.
    maintenance::Schedule schedule = createSchedule(
        {createWindow({machine1, machine2}, unavailability)});

    AWAIT_READY(registrar.apply(Owned<RegistryOperation>(
        new UpdateSchedule(schedule))));

    // Transition machine two into `DOWN` mode.
    RepeatedPtrField<MachineID> machines = createMachineList({machine2});

    AWAIT_READY(registrar.apply(Owned<RegistryOperation>(
        new StartMaintenance(machines))));
  }

  {
    // Check that machine two is down.
    Registrar registrar(flags, state);
    Future<Registry> registry = registrar.recover(master);
    AWAIT_READY(registry);

    EXPECT_EQ(2, registry->machines().machines().size());
    EXPECT_EQ(
        MachineInfo::DRAINING,
        registry->machines().machines(0).info().mode());

    EXPECT_EQ(
        MachineInfo::DOWN,
        registry->machines().machines(1).info().mode());

    // Schedule three machines for maintenance.
    maintenance::Schedule schedule = createSchedule(
        {createWindow({machine1, machine2, machine3}, unavailability)});

    AWAIT_READY(registrar.apply(Owned<RegistryOperation>(
        new UpdateSchedule(schedule))));

    // Deactivate the two `DRAINING` machines.
    RepeatedPtrField<MachineID> machines =
      createMachineList({machine1, machine3});

    AWAIT_READY(registrar.apply(Owned<RegistryOperation>(
        new StartMaintenance(machines))));
  }

  {
    // Check that all machines are down.
    Registrar registrar(flags, state);
    Future<Registry> registry = registrar.recover(master);
    AWAIT_READY(registry);

    EXPECT_EQ(3, registry->machines().machines().size());
    EXPECT_EQ(
        MachineInfo::DOWN,
        registry->machines().machines(0).info().mode());

    EXPECT_EQ(
        MachineInfo::DOWN,
        registry->machines().machines(1).info().mode());

    EXPECT_EQ(
        MachineInfo::DOWN,
        registry->machines().machines(2).info().mode());
  }
}


// Creates a schedule and properly starts and stops maintenance.
TEST_F(RegistrarTest, StopMaintenance)
{
  // Machine definitions used in this test.
  MachineID machine1;
  machine1.set_ip("0.0.0.1");

  MachineID machine2;
  machine2.set_hostname("2");

  MachineID machine3;
  machine3.set_hostname("3");
  machine3.set_ip("0.0.0.3");

  Unavailability unavailability = createUnavailability(Clock::now());

  {
    // Prepare the registrar.
    Registrar registrar(flags, state);
    AWAIT_READY(registrar.recover(master));

    // Schdule three machines for maintenance.
    maintenance::Schedule schedule = createSchedule({
        createWindow({machine1, machine2}, unavailability),
        createWindow({machine3}, unavailability)});

    AWAIT_READY(registrar.apply(Owned<RegistryOperation>(
        new UpdateSchedule(schedule))));

    // Transition machine three into `DOWN` mode.
    RepeatedPtrField<MachineID> machines = createMachineList({machine3});

    AWAIT_READY(registrar.apply(Owned<RegistryOperation>(
        new StartMaintenance(machines))));

    // Transition machine three into `UP` mode.
    AWAIT_READY(registrar.apply(Owned<RegistryOperation>(
        new StopMaintenance(machines))));
  }

  {
    // Check that machine three and the window were removed.
    Registrar registrar(flags, state);
    Future<Registry> registry = registrar.recover(master);
    AWAIT_READY(registry);

    EXPECT_EQ(1, registry->schedules().size());
    EXPECT_EQ(1, registry->schedules(0).windows().size());
    EXPECT_EQ(2, registry->schedules(0).windows(0).machine_ids().size());
    EXPECT_EQ(2, registry->machines().machines().size());
    EXPECT_EQ(
        MachineInfo::DRAINING,
        registry->machines().machines(0).info().mode());

    EXPECT_EQ(
        MachineInfo::DRAINING,
        registry->machines().machines(1).info().mode());

    // Transition machine one and two into `DOWN` mode.
    RepeatedPtrField<MachineID> machines =
      createMachineList({machine1, machine2});

    AWAIT_READY(registrar.apply(Owned<RegistryOperation>(
        new StartMaintenance(machines))));

    // Transition all machines into `UP` mode.
    AWAIT_READY(registrar.apply(Owned<RegistryOperation>(
        new StopMaintenance(machines))));
  }

  {
    // Check that the schedule is now empty.
    Registrar registrar(flags, state);
    Future<Registry> registry = registrar.recover(master);
    AWAIT_READY(registry);

    EXPECT_TRUE(registry->schedules().empty());
    EXPECT_TRUE(registry->machines().machines().empty());
  }
}


// Marks an agent for draining and checks for the appropriate data.
TEST_F(RegistrarTest, DrainAgent)
{
  SlaveID notAdmittedID;
  notAdmittedID.set_value("not-admitted");

  {
    // Prepare the registrar.
    Registrar registrar(flags, state);
    AWAIT_READY(registrar.recover(master));

    // Add an agent to be marked by other operations.
    AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
        new AdmitSlave(slave))));

    // Try to mark an unknown agent for draining.
    AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
        new DrainAgent(notAdmittedID, None(), false))));
  }

  {
    // Check that the agent is admitted, but has no DrainConfig.
    Registrar registrar(flags, state);
    Future<Registry> registry = registrar.recover(master);
    AWAIT_READY(registry);

    ASSERT_EQ(1, registry->slaves().slaves().size());
    EXPECT_EQ(slave, registry->slaves().slaves(0).info());
    EXPECT_FALSE(registry->slaves().slaves(0).has_drain_info());
    EXPECT_FALSE(registry->slaves().slaves(0).deactivated());

    // No minimum capability should be added when the operation does
    // not mutate anything.
    EXPECT_EQ(0, registry->minimum_capabilities().size());

    // Drain an admitted agent.
    AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
        new DrainAgent(slave.id(), None(), true))));
  }

  {
    // Check that agent is now marked for draining.
    Registrar registrar(flags, state);
    Future<Registry> registry = registrar.recover(master);
    AWAIT_READY(registry);

    ASSERT_EQ(1, registry->slaves().slaves().size());
    ASSERT_TRUE(registry->slaves().slaves(0).has_drain_info());
    EXPECT_EQ(DRAINING, registry->slaves().slaves(0).drain_info().state());
    EXPECT_FALSE(registry->slaves().slaves(0)
      .drain_info().config().has_max_grace_period());
    EXPECT_TRUE(registry->slaves().slaves(0).drain_info().config().mark_gone());
    EXPECT_TRUE(registry->slaves().slaves(0).deactivated());

    // Minimum capability should be added now.
    ASSERT_EQ(1, registry->minimum_capabilities().size());
    EXPECT_EQ(
        MasterInfo_Capability_Type_Name(MasterInfo::Capability::AGENT_DRAINING),
        registry->minimum_capabilities(0).capability());

    // Mark the agent unreachable.
    AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
        new MarkSlaveUnreachable(slave, protobuf::getCurrentTime()))));
  }

  {
    // Check that unreachable agent retains the draining.
    Registrar registrar(flags, state);
    Future<Registry> registry = registrar.recover(master);
    AWAIT_READY(registry);

    EXPECT_EQ(0, registry->slaves().slaves().size());
    ASSERT_EQ(1, registry->unreachable().slaves().size());
    ASSERT_TRUE(registry->unreachable().slaves(0).has_drain_info());
    EXPECT_FALSE(
        registry->unreachable().slaves(0)
          .drain_info().config().has_max_grace_period());
    EXPECT_TRUE(
        registry->unreachable().slaves(0).drain_info().config().mark_gone());
    EXPECT_TRUE(registry->unreachable().slaves(0).deactivated());

    // Mark the agent reachable.
    AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
        new MarkSlaveReachable(slave))));
  }

  {
    // Check that reachable agent retains the draining.
    Registrar registrar(flags, state);
    Future<Registry> registry = registrar.recover(master);
    AWAIT_READY(registry);

    ASSERT_EQ(1, registry->slaves().slaves().size());
    EXPECT_EQ(0, registry->unreachable().slaves().size());
    ASSERT_TRUE(registry->slaves().slaves(0).has_drain_info());
    EXPECT_FALSE(
        registry->slaves().slaves(0)
          .drain_info().config().has_max_grace_period());
    EXPECT_TRUE(registry->slaves().slaves(0).drain_info().config().mark_gone());
    EXPECT_TRUE(registry->slaves().slaves(0).deactivated());
  }
}


TEST_F(RegistrarTest, MarkAgentDrained)
{
  {
    // Prepare the registrar.
    Registrar registrar(flags, state);
    AWAIT_READY(registrar.recover(master));

    // Add an agent to be marked for draining.
    AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
        new AdmitSlave(slave))));

    // Try to mark a non-draining agent as drained. This should fail.
    AWAIT_FALSE(registrar.apply(Owned<RegistryOperation>(
        new MarkAgentDrained(slave.id()))));

    // Drain the admitted agent.
    AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
        new DrainAgent(slave.id(), None(), true))));
  }

  {
    // Check that agent is now marked for draining.
    Registrar registrar(flags, state);
    Future<Registry> registry = registrar.recover(master);
    AWAIT_READY(registry);

    ASSERT_EQ(1, registry->slaves().slaves().size());
    EXPECT_TRUE(registry->slaves().slaves(0).has_drain_info());
    EXPECT_EQ(DRAINING, registry->slaves().slaves(0).drain_info().state());

    // Transition from draining to drained.
    AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
        new MarkAgentDrained(slave.id()))));
  }

  {
    // Check that agent is now marked drained.
    Registrar registrar(flags, state);
    Future<Registry> registry = registrar.recover(master);
    AWAIT_READY(registry);

    ASSERT_EQ(1, registry->slaves().slaves().size());
    ASSERT_TRUE(registry->slaves().slaves(0).has_drain_info());
    EXPECT_EQ(DRAINED, registry->slaves().slaves(0).drain_info().state());

    // Try the same sequence of operations for an unreachable agent.
    // First remove the agent.
    AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
        new RemoveSlave(slave))));

    // Add the agent back, anew.
    AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
        new AdmitSlave(slave))));

    // Mark it unreachable.
    AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
        new MarkSlaveUnreachable(slave, protobuf::getCurrentTime()))));

    // Try to mark the agent drained prematurely.
    AWAIT_FALSE(registrar.apply(Owned<RegistryOperation>(
        new MarkAgentDrained(slave.id()))));

    // Now drain properly.
    AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
        new DrainAgent(slave.id(), None(), true))));

    // And finish draining.
    AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
        new MarkAgentDrained(slave.id()))));
  }

  {
    // Check that unreachable agent is now marked drained.
    Registrar registrar(flags, state);
    Future<Registry> registry = registrar.recover(master);
    AWAIT_READY(registry);

    ASSERT_EQ(1, registry->unreachable().slaves().size());
    ASSERT_TRUE(registry->unreachable().slaves(0).has_drain_info());
    EXPECT_EQ(DRAINED, registry->unreachable().slaves(0).drain_info().state());
  }
}


TEST_F(RegistrarTest, DeactivateAgent)
{
  SlaveID notAdmittedID;
  notAdmittedID.set_value("not-admitted");

  {
    // Prepare the registrar.
    Registrar registrar(flags, state);
    AWAIT_READY(registrar.recover(master));

    // Add an agent to be marked by other operations.
    AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
        new AdmitSlave(slave))));

    // Try to mark an unknown agent for draining.
    AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
        new DeactivateAgent(notAdmittedID))));
  }

  {
    // Check that the agent is admitted and is not deactivated.
    Registrar registrar(flags, state);
    Future<Registry> registry = registrar.recover(master);
    AWAIT_READY(registry);

    ASSERT_EQ(1, registry->slaves().slaves().size());
    EXPECT_EQ(slave, registry->slaves().slaves(0).info());
    EXPECT_FALSE(registry->slaves().slaves(0).has_drain_info());
    EXPECT_FALSE(registry->slaves().slaves(0).deactivated());

    // No minimum capability should be added when the operation does
    // not mutate anything.
    EXPECT_EQ(0, registry->minimum_capabilities().size());

    // Deactivate the admitted agent this time.
    AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
        new DeactivateAgent(slave.id()))));
  }

  {
    // Check that agent is now deactivated.
    Registrar registrar(flags, state);
    Future<Registry> registry = registrar.recover(master);
    AWAIT_READY(registry);

    ASSERT_EQ(1, registry->slaves().slaves().size());
    EXPECT_FALSE(registry->slaves().slaves(0).has_drain_info());
    EXPECT_TRUE(registry->slaves().slaves(0).deactivated());

    // Minimum capability should be added now.
    ASSERT_EQ(1, registry->minimum_capabilities().size());
    EXPECT_EQ(
        MasterInfo_Capability_Type_Name(MasterInfo::Capability::AGENT_DRAINING),
        registry->minimum_capabilities(0).capability());

    // Mark the agent unreachable.
    AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
        new MarkSlaveUnreachable(slave, protobuf::getCurrentTime()))));
  }

  {
    // Check that unreachable agent retains the deactivation.
    Registrar registrar(flags, state);
    Future<Registry> registry = registrar.recover(master);
    AWAIT_READY(registry);

    EXPECT_EQ(0, registry->slaves().slaves().size());
    ASSERT_EQ(1, registry->unreachable().slaves().size());
    EXPECT_FALSE(registry->unreachable().slaves(0).has_drain_info());
    EXPECT_TRUE(registry->unreachable().slaves(0).deactivated());

    // Mark the agent reachable.
    AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
        new MarkSlaveReachable(slave))));
  }

  {
    // Check that reachable agent retains the deactivation.
    Registrar registrar(flags, state);
    Future<Registry> registry = registrar.recover(master);
    AWAIT_READY(registry);

    ASSERT_EQ(1, registry->slaves().slaves().size());
    EXPECT_EQ(0, registry->unreachable().slaves().size());
    EXPECT_FALSE(registry->slaves().slaves(0).has_drain_info());
    EXPECT_TRUE(registry->slaves().slaves(0).deactivated());
  }
}


// Checks that reactivating agents will remove the draining/deactivated
// metadata and the AGENT_DRAINING minimum capability correctly.
TEST_F(RegistrarTest, ReactivateAgent)
{
  SlaveID reachable1;
  reachable1.set_value("reachable1");

  SlaveID reachable2;
  reachable2.set_value("reachable2");

  SlaveID unreachable1;
  unreachable1.set_value("unreachable1");

  SlaveID unreachable2;
  unreachable2.set_value("unreachable2");

  SlaveInfo info1;
  info1.set_hostname("localhost");
  info1.mutable_id()->CopyFrom(reachable1);

  SlaveInfo info2;
  info2.set_hostname("localhost");
  info2.mutable_id()->CopyFrom(reachable2);

  SlaveInfo info3;
  info3.set_hostname("localhost");
  info3.mutable_id()->CopyFrom(unreachable1);

  SlaveInfo info4;
  info4.set_hostname("localhost");
  info4.mutable_id()->CopyFrom(unreachable2);

  {
    // Prepare the registrar.
    Registrar registrar(flags, state);
    AWAIT_READY(registrar.recover(master));

    // Add all the agents.
    AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
        new AdmitSlave(info1))));
    AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
        new AdmitSlave(info2))));
    AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
        new AdmitSlave(info3))));
    AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
        new AdmitSlave(info4))));

    // Mark two agents as unreachable.
    AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
        new MarkSlaveUnreachable(info3, protobuf::getCurrentTime()))));
    AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
        new MarkSlaveUnreachable(info4, protobuf::getCurrentTime()))));

    // Two reachable deactivated agents.
    AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
        new DeactivateAgent(reachable1))));
    AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
        new DeactivateAgent(reachable2))));
  }

  {
    // Check for two deactivated agents and the minimum capability.
    Registrar registrar(flags, state);
    Future<Registry> registry = registrar.recover(master);
    AWAIT_READY(registry);

    ASSERT_EQ(2, registry->slaves().slaves().size());
    ASSERT_EQ(2, registry->unreachable().slaves().size());

    EXPECT_TRUE(registry->slaves().slaves(0).deactivated());
    EXPECT_TRUE(registry->slaves().slaves(1).deactivated());

    ASSERT_EQ(1, registry->minimum_capabilities().size());
    EXPECT_EQ(
        MasterInfo_Capability_Type_Name(MasterInfo::Capability::AGENT_DRAINING),
        registry->minimum_capabilities(0).capability());

    // Reactivate one agent.
    AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
        new ReactivateAgent(reachable1))));
  }

  {
    // Check for one deactivated agent and
    // that the minimum capability is still present.
    Registrar registrar(flags, state);
    Future<Registry> registry = registrar.recover(master);
    AWAIT_READY(registry);

    ASSERT_EQ(2, registry->slaves().slaves().size());
    ASSERT_EQ(2, registry->unreachable().slaves().size());

    EXPECT_FALSE(registry->slaves().slaves(0).deactivated());
    EXPECT_TRUE(registry->slaves().slaves(1).deactivated());

    ASSERT_EQ(1, registry->minimum_capabilities().size());
    EXPECT_EQ(
        MasterInfo_Capability_Type_Name(MasterInfo::Capability::AGENT_DRAINING),
        registry->minimum_capabilities(0).capability());

    // Reactivate the other agent.
    AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
        new ReactivateAgent(reachable2))));
  }

  {
    // Check for one deactivated agent and
    // that the minimum capability is still present.
    Registrar registrar(flags, state);
    Future<Registry> registry = registrar.recover(master);
    AWAIT_READY(registry);

    ASSERT_EQ(2, registry->slaves().slaves().size());
    ASSERT_EQ(2, registry->unreachable().slaves().size());

    EXPECT_FALSE(registry->slaves().slaves(0).deactivated());
    EXPECT_FALSE(registry->slaves().slaves(1).deactivated());

    ASSERT_EQ(0, registry->minimum_capabilities().size());

    // Two unreachable deactivated agents.
    AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
        new DeactivateAgent(unreachable1))));
    AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
        new DeactivateAgent(unreachable2))));

    // Try reactivating an agent that is already active.
    // This should not result in a change.
    AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
        new ReactivateAgent(reachable1))));
  }

  {
    // Again, check for two deactivated agents and the minimum capability.
    Registrar registrar(flags, state);
    Future<Registry> registry = registrar.recover(master);
    AWAIT_READY(registry);

    ASSERT_EQ(2, registry->slaves().slaves().size());
    ASSERT_EQ(2, registry->unreachable().slaves().size());

    EXPECT_TRUE(registry->unreachable().slaves(0).deactivated());
    EXPECT_TRUE(registry->unreachable().slaves(1).deactivated());

    ASSERT_EQ(1, registry->minimum_capabilities().size());
    EXPECT_EQ(
        MasterInfo_Capability_Type_Name(MasterInfo::Capability::AGENT_DRAINING),
        registry->minimum_capabilities(0).capability());

    // Reactivate one. This time, in the opposite order.
    AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
        new ReactivateAgent(unreachable2))));
  }

  {
    // Should be one deactivated agent, with minimum capability still present.
    Registrar registrar(flags, state);
    Future<Registry> registry = registrar.recover(master);
    AWAIT_READY(registry);

    ASSERT_EQ(2, registry->slaves().slaves().size());
    ASSERT_EQ(2, registry->unreachable().slaves().size());

    EXPECT_TRUE(registry->unreachable().slaves(0).deactivated());
    EXPECT_FALSE(registry->unreachable().slaves(1).deactivated());

    ASSERT_EQ(1, registry->minimum_capabilities().size());
    EXPECT_EQ(
        MasterInfo_Capability_Type_Name(MasterInfo::Capability::AGENT_DRAINING),
        registry->minimum_capabilities(0).capability());

    // Reactivate the other one.
    AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
        new ReactivateAgent(unreachable1))));
  }

  {
    // No deactivated agents, with no minimum capability.
    Registrar registrar(flags, state);
    Future<Registry> registry = registrar.recover(master);
    AWAIT_READY(registry);

    ASSERT_EQ(2, registry->slaves().slaves().size());
    ASSERT_EQ(2, registry->unreachable().slaves().size());

    EXPECT_FALSE(registry->unreachable().slaves(0).deactivated());
    EXPECT_FALSE(registry->unreachable().slaves(1).deactivated());

    ASSERT_EQ(0, registry->minimum_capabilities().size());

    // Now try deactivate reachable and unreachable together.
    AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
        new DeactivateAgent(reachable1))));
    AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
        new DeactivateAgent(unreachable1))));

    // We'll skip a validation step here and reactivate one right away.
    AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
        new ReactivateAgent(reachable1))));
  }

  {
    // Minimum capability should not be removed if an unreachable agent
    // is deactivated.
    Registrar registrar(flags, state);
    Future<Registry> registry = registrar.recover(master);
    AWAIT_READY(registry);

    ASSERT_EQ(2, registry->slaves().slaves().size());
    ASSERT_EQ(2, registry->unreachable().slaves().size());

    EXPECT_FALSE(registry->slaves().slaves(0).deactivated());
    EXPECT_TRUE(registry->unreachable().slaves(0).deactivated());

    ASSERT_EQ(1, registry->minimum_capabilities().size());
    EXPECT_EQ(
        MasterInfo_Capability_Type_Name(MasterInfo::Capability::AGENT_DRAINING),
        registry->minimum_capabilities(0).capability());
  }
}


// Tests that adding and updating quotas in the registry works properly.
TEST_F(RegistrarTest, UpdateQuota)
{
  // Helper to construct `QuotaConfig`.
  auto createQuotaConfig = [](const string& role,
                              const string& quantitiesString,
                              const string& limitsString) {
    QuotaConfig config;
    config.set_role(role);

    google::protobuf::Map<string, Value::Scalar> guarantees_;
    ResourceQuantities quantities =
      CHECK_NOTERROR(ResourceQuantities::fromString(quantitiesString));
    foreachpair (const string& name, const Value::Scalar& scalar, quantities) {
      guarantees_[name] = scalar;
    }

    google::protobuf::Map<string, Value::Scalar> limits_;
    ResourceLimits limits =
      CHECK_NOTERROR(ResourceLimits::fromString(limitsString));
    foreachpair (const string& name, const Value::Scalar& scalar, limits) {
      limits_[name] = scalar;
    }

    *config.mutable_guarantees() = std::move(guarantees_);
    *config.mutable_limits() = std::move(limits_);

    return config;
  };

  RepeatedPtrField<QuotaConfig> configs;

  {
    // Initially no quota and minimum capabilities are recorded.

    Registrar registrar(flags, state);
    Future<Registry> registry = registrar.recover(master);
    AWAIT_READY(registry);

    EXPECT_EQ(0, registry->quota_configs().size());
    EXPECT_EQ(0, registry->minimum_capabilities().size());

    // Store quota for a role with default quota.
    *configs.Add() = createQuotaConfig("role1", "", "");

    AWAIT_TRUE(
        registrar.apply(Owned<RegistryOperation>(new UpdateQuota(configs))));
  }

  {
    Registrar registrar(flags, state);
    Future<Registry> registry = registrar.recover(master);
    AWAIT_READY(registry);

    // Default quota is not persisted into the registry.
    EXPECT_EQ(0, registry->quota_configs().size());
    EXPECT_EQ(0, registry->minimum_capabilities().size());

    // Update quota for `role1`.
    configs.Clear();
    *configs.Add() =
      createQuotaConfig("role1", "cpus:1;mem:1024", "cpus:2;mem:2048");

    AWAIT_TRUE(
        registrar.apply(Owned<RegistryOperation>(new UpdateQuota(configs))));
  }

  {
    Registrar registrar(flags, state);
    Future<Registry> registry = registrar.recover(master);
    AWAIT_READY(registry);

    EXPECT_EQ(1, registry->quota_configs().size());

    Try<JSON::Array> expected = JSON::parse<JSON::Array>(
      R"~(
      [
        {
          "guarantees": {
            "cpus": {
              "value": 1
            },
            "mem": {
              "value": 1024
            }
          },
          "limits": {
            "cpus": {
              "value": 2
            },
            "mem": {
              "value": 2048
            }
          },
          "role": "role1"
        }
      ])~");

    EXPECT_EQ(
        CHECK_NOTERROR(expected), JSON::protobuf(registry->quota_configs()));

    // The `QUOTA_V2` capability is added to the registry.
    //
    // TODO(mzhu): This assumes the the registry starts empty which might not
    // be in the future. Just check the presence of `QUOTA_V2`.
    EXPECT_EQ(1, registry->minimum_capabilities().size());
    EXPECT_EQ("QUOTA_V2", registry->minimum_capabilities(0).capability());

    // Update quota for "role2".
    configs.Clear();
    *configs.Add() =
      createQuotaConfig("role2", "cpus:1;mem:1024", "cpus:2;mem:2048");

    AWAIT_TRUE(
        registrar.apply(Owned<RegistryOperation>(new UpdateQuota(configs))));
  }

  {
    Registrar registrar(flags, state);
    Future<Registry> registry = registrar.recover(master);
    AWAIT_READY(registry);

    // Check that the recovered quotas match those we stored previously.
    // NOTE: We assume quota messages are stored in order they have
    // been added.
    // TODO(alexr): Consider removing dependency on the order.
    Try<JSON::Array> expected = JSON::parse<JSON::Array>(
      R"~(
      [
        {
          "guarantees": {
            "cpus": {
              "value": 1
            },
            "mem": {
              "value": 1024
            }
          },
          "limits": {
            "cpus": {
              "value": 2
            },
            "mem": {
              "value": 2048
            }
          },
          "role": "role1"
        },
        {
          "guarantees": {
            "cpus": {
              "value": 1
            },
            "mem": {
              "value": 1024
            }
          },
          "limits": {
            "cpus": {
              "value": 2
            },
            "mem": {
              "value": 2048
            }
          },
          "role": "role2"
        }
      ])~");

    EXPECT_EQ(
        CHECK_NOTERROR(expected), JSON::protobuf(registry->quota_configs()));

    // TODO(mzhu): This assumes the the registry starts empty which might not
    // be in the future. Just check the presence of `QUOTA_V2`.
    EXPECT_EQ(1, registry->minimum_capabilities().size());
    EXPECT_EQ("QUOTA_V2", registry->minimum_capabilities(0).capability());

    // Change quota for "role1"` and "role2"` in a single call.
    configs.Clear();
    *configs.Add() = createQuotaConfig("role1", "cpus:2", "cpus:4");
    *configs.Add() = createQuotaConfig("role2", "cpus:2", "cpus:4");

    AWAIT_TRUE(
        registrar.apply(Owned<RegistryOperation>(new UpdateQuota(configs))));
  }

  {
    Registrar registrar(flags, state);
    Future<Registry> registry = registrar.recover(master);
    AWAIT_READY(registry);

    // Check that the recovered quotas match those we stored previously.
    // NOTE: We assume quota messages are stored in order they have
    // been added.
    // TODO(alexr): Consider removing dependency on the order.
    Try<JSON::Array> expected = JSON::parse<JSON::Array>(
      R"~(
      [
        {
          "guarantees": {
            "cpus": {
              "value": 2
            }
          },
          "limits": {
            "cpus": {
              "value": 4
            }
          },
          "role": "role1"
        },
        {
          "guarantees": {
            "cpus": {
              "value": 2
            }
          },
          "limits": {
            "cpus": {
              "value": 4
            }
          },
          "role": "role2"
        }
      ])~");

    EXPECT_EQ(
        CHECK_NOTERROR(expected), JSON::protobuf(registry->quota_configs()));

    // TODO(mzhu): This assumes the the registry starts empty which might not
    // be in the future. Just check the presence of `QUOTA_V2`.
    EXPECT_EQ(1, registry->minimum_capabilities().size());
    EXPECT_EQ("QUOTA_V2", registry->minimum_capabilities(0).capability());

    // Reset "role2"` quota to default.
    configs.Clear();
    *configs.Add() = createQuotaConfig("role2", "", "");

    AWAIT_TRUE(
        registrar.apply(Owned<RegistryOperation>(new UpdateQuota(configs))));
  }

  {
    Registrar registrar(flags, state);
    Future<Registry> registry = registrar.recover(master);
    AWAIT_READY(registry);

    configs.Clear();
    *configs.Add() = createQuotaConfig("role1", "cpus:2", "cpus:4");

    Try<JSON::Array> expected = JSON::parse<JSON::Array>(
      R"~(
      [
        {
          "guarantees": {
            "cpus": {
              "value": 2
            }
          },
          "limits": {
            "cpus": {
              "value": 4
            }
          },
          "role": "role1"
        }
      ])~");

    EXPECT_EQ(
        CHECK_NOTERROR(expected), JSON::protobuf(registry->quota_configs()));

    // TODO(mzhu): This assumes the the registry starts empty which might not
    // be in the future. Just check the presence of `QUOTA_V2`.
    EXPECT_EQ(1, registry->minimum_capabilities().size());
    EXPECT_EQ("QUOTA_V2", registry->minimum_capabilities(0).capability());

    // Reset "role1"` quota to default.
    configs.Clear();
    *configs.Add() = createQuotaConfig("role1", "", "");

    AWAIT_TRUE(
        registrar.apply(Owned<RegistryOperation>(new UpdateQuota(configs))));
  }

  {
    Registrar registrar(flags, state);
    Future<Registry> registry = registrar.recover(master);
    AWAIT_READY(registry);

    EXPECT_EQ(0, registry->quota_configs().size());
    // The `QUOTA_V2` capability is removed because `quota_configs` is empty.
    EXPECT_EQ(0, registry->minimum_capabilities().size());
  }
}


// Tests that updating quota with an invalid config fails.
TEST_F(RegistrarTest, UpdateQuotaInvalid)
{
  QuotaConfig config;
  config.set_role("role1");

  auto resourceMap = [](const vector<pair<string, double>>& vector)
    -> Map<string, Value::Scalar> {
    Map<string, Value::Scalar> result;

    foreachpair (const string& name, double value, vector) {
      Value::Scalar scalar;
      scalar.set_value(value);
      result[name] = scalar;
    }

    return result;
  };

  // The quota endpoint only allows memory / disk up to
  // 1 exabyte (in megabytes) or 1 trillion cores/ports/other.
  // For this test, we just check 1 invalid case via mem.
  double largestMegabytes = 1024.0 * 1024.0 * 1024.0 * 1024.0;

  *config.mutable_limits() = resourceMap({{"mem", largestMegabytes + 1.0}});

  Registrar registrar(flags, state);
  Future<Registry> registry = registrar.recover(master);
  AWAIT_READY(registry);

  EXPECT_EQ(0, registry->quota_configs().size());
  EXPECT_EQ(0, registry->minimum_capabilities().size());

  // Store quota for a role with default quota.
  RepeatedPtrField<QuotaConfig> configs;
  *configs.Add() = config;

  AWAIT_FALSE(
      registrar.apply(Owned<RegistryOperation>(new UpdateQuota(configs))));
}


// Tests that updating weights in the registry works properly.
TEST_F(RegistrarTest, UpdateWeights)
{
  const string ROLE1 = "role1";
  double WEIGHT1 = 2.0;
  double UPDATED_WEIGHT1 = 1.0;

  const string ROLE2 = "role2";
  double WEIGHT2 = 3.5;

  {
    // Prepare the registrar.
    Registrar registrar(flags, state);
    Future<Registry> registry = registrar.recover(master);
    AWAIT_READY(registry);

    ASSERT_TRUE(registry->weights().empty());

    // Store the weight for 'ROLE1' previously without weight.
    hashmap<string, double> weights;
    weights[ROLE1] = WEIGHT1;
    vector<WeightInfo> weightInfos = getWeightInfos(weights);

    AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
        new UpdateWeights(weightInfos))));
  }

  {
    Registrar registrar(flags, state);
    Future<Registry> registry = registrar.recover(master);
    AWAIT_READY(registry);

    // Check that the recovered weights matches the weights we stored
    // previously.
    ASSERT_EQ(1, registry->weights_size());
    EXPECT_EQ(ROLE1, registry->weights(0).info().role());
    ASSERT_EQ(WEIGHT1, registry->weights(0).info().weight());

    // Change weight for 'ROLE1', and store the weight for 'ROLE2'.
    hashmap<string, double> weights;
    weights[ROLE1] = UPDATED_WEIGHT1;
    weights[ROLE2] = WEIGHT2;
    vector<WeightInfo> weightInfos = getWeightInfos(weights);

    AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
        new UpdateWeights(weightInfos))));
  }

  {
    Registrar registrar(flags, state);
    Future<Registry> registry = registrar.recover(master);
    AWAIT_READY(registry);

    // Check that the recovered weights matches the weights we updated
    // previously.
    ASSERT_EQ(2, registry->weights_size());
    EXPECT_EQ(ROLE1, registry->weights(0).info().role());
    ASSERT_EQ(UPDATED_WEIGHT1, registry->weights(0).info().weight());
    EXPECT_EQ(ROLE2, registry->weights(1).info().role());
    ASSERT_EQ(WEIGHT2, registry->weights(1).info().weight());
  }
}


TEST_F(RegistrarTest, Bootstrap)
{
  // Run 1 simulates the reregistration of a slave that is not present
  // in the registry.
  {
    Registrar registrar(flags, state);
    AWAIT_READY(registrar.recover(master));

    AWAIT_TRUE(registrar.apply(Owned<RegistryOperation>(
        new MarkSlaveReachable(slave))));
  }

  // Run 2 should see the slave.
  {
    Registrar registrar(flags, state);

    Future<Registry> registry = registrar.recover(master);

    AWAIT_READY(registry);

    ASSERT_EQ(1, registry->slaves().slaves().size());
    EXPECT_EQ(slave, registry->slaves().slaves(0).info());
  }
}


class MockStorage : public Storage
{
public:
  MOCK_METHOD1(get, Future<Option<Entry>>(const string&));
  MOCK_METHOD2(set, Future<bool>(const Entry&, const id::UUID&));
  MOCK_METHOD1(expunge, Future<bool>(const Entry&));
  MOCK_METHOD0(names, Future<std::set<string>>());
};


TEST_F(RegistrarTest, FetchTimeout)
{
  Clock::pause();

  MockStorage storage;
  State state(&storage);

  Future<Nothing> get;
  EXPECT_CALL(storage, get(_))
    .WillOnce(DoAll(FutureSatisfy(&get),
                    Return(Future<Option<Entry>>())));

  Registrar registrar(flags, &state);

  Future<Registry> recover = registrar.recover(master);

  AWAIT_READY(get);

  Clock::advance(flags.registry_fetch_timeout);

  AWAIT_FAILED(recover);

  Clock::resume();

  // Ensure the registrar fails subsequent operations.
  AWAIT_FAILED(registrar.apply(Owned<RegistryOperation>(
      new AdmitSlave(slave))));
}


TEST_F(RegistrarTest, StoreTimeout)
{
  Clock::pause();

  MockStorage storage;
  State state(&storage);

  Registrar registrar(flags, &state);

  EXPECT_CALL(storage, get(_))
    .WillOnce(Return(None()));

  Future<Nothing> set;
  EXPECT_CALL(storage, set(_, _))
    .WillOnce(DoAll(FutureSatisfy(&set),
                    Return(Future<bool>())));

  Future<Registry> recover = registrar.recover(master);

  AWAIT_READY(set);

  Clock::advance(flags.registry_store_timeout);

  AWAIT_FAILED(recover);

  Clock::resume();

  // Ensure the registrar fails subsequent operations.
  AWAIT_FAILED(registrar.apply(Owned<RegistryOperation>(
      new AdmitSlave(slave))));
}


TEST_F(RegistrarTest, Abort)
{
  MockStorage storage;
  State state(&storage);

  Registrar registrar(flags, &state);

  EXPECT_CALL(storage, get(_))
    .WillOnce(Return(None()));

  EXPECT_CALL(storage, set(_, _))
    .WillOnce(Return(Future<bool>(true)))              // Recovery.
    .WillOnce(Return(Future<bool>::failed("failure"))) // Failure.
    .WillRepeatedly(Return(Future<bool>(true)));       // Success.

  AWAIT_READY(registrar.recover(master));

  // Storage failure.
  AWAIT_FAILED(registrar.apply(Owned<RegistryOperation>(
      new AdmitSlave(slave))));

  // The registrar should now be aborted!
  AWAIT_FAILED(registrar.apply(Owned<RegistryOperation>(
      new AdmitSlave(slave))));
}


// Tests that requests to the '/registry' endpoint are authenticated when HTTP
// authentication is enabled.
TEST_F(RegistrarTest, Authentication)
{
  const string AUTHENTICATION_REALM = "realm";

  Credentials credentials;
  credentials.add_credentials()->CopyFrom(DEFAULT_CREDENTIAL);

  Try<authentication::Authenticator*> authenticator =
    BasicAuthenticatorFactory::create(AUTHENTICATION_REALM, credentials);

  ASSERT_SOME(authenticator);

  AWAIT_READY(authentication::setAuthenticator(
      AUTHENTICATION_REALM,
      Owned<authentication::Authenticator>(authenticator.get())));

  Registrar registrar(flags, state, AUTHENTICATION_REALM);

  // Requests without credentials should be rejected.
  Future<Response> response = process::http::get(registrar.pid(), "registry");
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response);

  Credential badCredential;
  badCredential.set_principal("bad-principal");
  badCredential.set_secret("bad-secret");

  // Requests with bad credentials should be rejected.
  response = process::http::get(
      registrar.pid(),
      "registry",
      None(),
      createBasicAuthHeaders(badCredential));
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response);

  // Requests with good credentials should be permitted.
  response = process::http::get(
      registrar.pid(),
      "registry",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  AWAIT_READY(authentication::unsetAuthenticator(AUTHENTICATION_REALM));
}


class Registrar_BENCHMARK_Test
  : public RegistrarTestBase,
    public WithParamInterface<size_t> {};


// The Registrar benchmark tests are parameterized by the number of slaves.
INSTANTIATE_TEST_CASE_P(
    SlaveCount,
    Registrar_BENCHMARK_Test,
    ::testing::Values(10000U, 20000U, 30000U, 50000U));


TEST_P(Registrar_BENCHMARK_Test, Performance)
{
  Registrar registrar(flags, state);
  AWAIT_READY(registrar.recover(master));

  Attributes attributes = Attributes::parse("foo:bar;baz:quux");
  Resources resources =
    Resources::parse("cpus(*):1.0;mem(*):512;disk(*):2048").get();

  size_t slaveCount = GetParam();

  // Create slaves.
  vector<SlaveInfo> infos;
  for (size_t i = 0; i < slaveCount; ++i) {
    // Simulate real slave information.
    SlaveInfo info;
    info.set_hostname("localhost");
    info.mutable_id()->set_value(
        string("201310101658-2280333834-5050-48574-") + stringify(i));
    info.mutable_resources()->MergeFrom(resources);
    info.mutable_attributes()->MergeFrom(attributes);
    infos.push_back(info);
  }

  // Admit slaves.
  Stopwatch watch;
  watch.start();
  Future<bool> result;
  foreach (const SlaveInfo& info, infos) {
    result = registrar.apply(Owned<RegistryOperation>(new AdmitSlave(info)));
  }
  AWAIT_READY_FOR(result, Minutes(5));
  LOG(INFO) << "Admitted " << slaveCount << " agents in " << watch.elapsed();

  // Shuffle the slaves so we are readmitting them in random order (
  // same as in production).
  std::random_shuffle(infos.begin(), infos.end());

  // Mark slaves reachable again. This simulates the master failing
  // over, and then the previously registered agents reregistering
  // with the new master.
  watch.start();
  foreach (const SlaveInfo& info, infos) {
    result = registrar.apply(Owned<RegistryOperation>(
        new MarkSlaveReachable(info)));
  }
  AWAIT_READY_FOR(result, Minutes(5));
  LOG(INFO) << "Marked " << slaveCount
            << " agents reachable in " << watch.elapsed();

  // Recover slaves.
  Registrar registrar2(flags, state);
  watch.start();
  MasterInfo info;
  info.set_id("master");
  info.set_ip(10000000);
  info.set_port(5050);
  Future<Registry> registry = registrar2.recover(info);
  AWAIT_READY(registry);
  LOG(INFO) << "Recovered " << slaveCount << " agents ("
            << Bytes(registry->ByteSize()) << ") in " << watch.elapsed();

  // Shuffle the slaves so we are removing them in random order (same
  // as in production).
  std::random_shuffle(infos.begin(), infos.end());

  // Remove slaves.
  watch.start();
  foreach (const SlaveInfo& info, infos) {
    result = registrar2.apply(Owned<RegistryOperation>(new RemoveSlave(info)));
  }
  AWAIT_READY_FOR(result, Minutes(5));
  cout << "Removed " << slaveCount << " agents in " << watch.elapsed() << endl;
}


// Test the performance of marking all registered slaves unreachable,
// then marking them reachable again. This might occur if there is a
// network partition and then the partition heals.
TEST_P(Registrar_BENCHMARK_Test, MarkUnreachableThenReachable)
{
  Registrar registrar(flags, state);
  AWAIT_READY(registrar.recover(master));

  Attributes attributes = Attributes::parse("foo:bar;baz:quux");
  Resources resources =
    Resources::parse("cpus(*):1.0;mem(*):512;disk(*):2048").get();

  size_t slaveCount = GetParam();

  // Create slaves.
  vector<SlaveInfo> infos;
  for (size_t i = 0; i < slaveCount; ++i) {
    // Simulate real slave information.
    SlaveInfo info;
    info.set_hostname("localhost");
    info.mutable_id()->set_value(
        string("201310101658-2280333834-5050-48574-") + stringify(i));
    info.mutable_resources()->MergeFrom(resources);
    info.mutable_attributes()->MergeFrom(attributes);
    infos.push_back(info);
  }

  // Admit slaves.
  Stopwatch watch;
  watch.start();
  Future<bool> result;
  foreach (const SlaveInfo& info, infos) {
    result = registrar.apply(Owned<RegistryOperation>(new AdmitSlave(info)));
  }
  AWAIT_READY_FOR(result, Minutes(5));
  LOG(INFO) << "Admitted " << slaveCount << " agents in " << watch.elapsed();

  // Shuffle the slaves so that we mark them unreachable in random
  // order (same as in production).
  std::random_shuffle(infos.begin(), infos.end());

  // Mark all slaves unreachable.
  TimeInfo unreachableTime = protobuf::getCurrentTime();

  watch.start();
  foreach (const SlaveInfo& info, infos) {
    result = registrar.apply(Owned<RegistryOperation>(
        new MarkSlaveUnreachable(info, unreachableTime)));
  }
  AWAIT_READY_FOR(result, Minutes(5));
  cout << "Marked " << slaveCount << " agents unreachable in "
       << watch.elapsed() << endl;

  // Shuffles the slaves again so that we mark them reachable in
  // random order (same as in production).
  std::random_shuffle(infos.begin(), infos.end());

  // Mark all slaves reachable.
  watch.start();
  foreach (const SlaveInfo& info, infos) {
    result = registrar.apply(Owned<RegistryOperation>(
        new MarkSlaveReachable(info)));
  }
  AWAIT_READY_FOR(result, Minutes(5));
  cout << "Marked " << slaveCount << " agents reachable in "
       << watch.elapsed() << endl;
}


// Test the performance of garbage collecting a large portion of the
// unreachable list in a single operation. We use a fixed percentage
// at the moment (50%).
TEST_P(Registrar_BENCHMARK_Test, GcManyAgents)
{
  Registrar registrar(flags, state);
  AWAIT_READY(registrar.recover(master));

  Attributes attributes = Attributes::parse("foo:bar;baz:quux");
  Resources resources =
    Resources::parse("cpus(*):1.0;mem(*):512;disk(*):2048").get();

  size_t slaveCount = GetParam();

  // Create slaves.
  vector<SlaveInfo> infos;
  for (size_t i = 0; i < slaveCount; ++i) {
    // Simulate real slave information.
    SlaveInfo info;
    info.set_hostname("localhost");
    info.mutable_id()->set_value(
        string("201310101658-2280333834-5050-48574-") + stringify(i));
    info.mutable_resources()->MergeFrom(resources);
    info.mutable_attributes()->MergeFrom(attributes);
    infos.push_back(info);
  }

  // Admit slaves.
  Stopwatch watch;
  watch.start();
  Future<bool> result;
  foreach (const SlaveInfo& info, infos) {
    result = registrar.apply(Owned<RegistryOperation>(new AdmitSlave(info)));
  }
  AWAIT_READY_FOR(result, Minutes(5));
  LOG(INFO) << "Admitted " << slaveCount << " agents in " << watch.elapsed();

  // Shuffle the slaves so that we mark them unreachable in random
  // order (same as in production).
  std::random_shuffle(infos.begin(), infos.end());

  // Mark all slaves unreachable.
  TimeInfo unreachableTime = protobuf::getCurrentTime();

  watch.start();
  foreach (const SlaveInfo& info, infos) {
    result = registrar.apply(Owned<RegistryOperation>(
        new MarkSlaveUnreachable(info, unreachableTime)));
  }
  AWAIT_READY_FOR(result, Minutes(5));
  LOG(INFO) << "Marked " << slaveCount << " agents unreachable in "
            << watch.elapsed() << endl;

  // Prepare to GC the first half of the unreachable list.
  hashset<SlaveID> toRemove;
  for (size_t i = 0; (i * 2) < slaveCount; i++) {
    const SlaveInfo& info = infos[i];
    toRemove.insert(info.id());
  }

  // Do GC.
  watch.start();
  result = registrar.apply(Owned<RegistryOperation>(
      new Prune(toRemove, hashset<SlaveID>())));

  AWAIT_READY_FOR(result, Minutes(5));
  cout << "Garbage collected " << toRemove.size() << " agents in "
       << watch.elapsed() << endl;
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
