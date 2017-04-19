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
#include "master/weights.hpp"

#include "tests/mesos.hpp"

using namespace mesos::internal::master;

using namespace process;

using mesos::log::Log;

using mesos::internal::log::Replica;

using std::cout;
using std::endl;
using std::map;
using std::set;
using std::string;
using std::vector;

using process::Clock;
using process::Owned;

using process::http::OK;
using process::http::Response;
using process::http::Unauthorized;

using google::protobuf::RepeatedPtrField;

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
  virtual void SetUp()
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
    flags.registry_store_timeout = Seconds(10);

    master.CopyFrom(protobuf::createMasterInfo(UPID("master@127.0.0.1:5050")));

    SlaveID id;
    id.set_value("1");

    SlaveInfo info;
    info.set_hostname("localhost");
    info.mutable_id()->CopyFrom(id);

    slave.CopyFrom(info);
  }

  virtual void TearDown()
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
  AWAIT_EXPECT_FAILED(
      registrar.apply(Owned<Operation>(new AdmitSlave(slave))));
  AWAIT_EXPECT_FAILED(
      registrar.apply(
          Owned<Operation>(
              new MarkSlaveUnreachable(slave, protobuf::getCurrentTime()))));
  AWAIT_EXPECT_FAILED(
      registrar.apply(Owned<Operation>(new MarkSlaveReachable(slave))));
  AWAIT_EXPECT_FAILED(
      registrar.apply(Owned<Operation>(new RemoveSlave(slave))));

  Future<Registry> registry = registrar.recover(master);

  // Before waiting for the recovery to complete, invoke some
  // operations to ensure they do not fail.
  Future<bool> admit = registrar.apply(
      Owned<Operation>(new AdmitSlave(slave)));
  Future<bool> unreachable = registrar.apply(
      Owned<Operation>(
          new MarkSlaveUnreachable(slave, protobuf::getCurrentTime())));
  Future<bool> reachable = registrar.apply(
      Owned<Operation>(new MarkSlaveReachable(slave)));
  Future<bool> remove = registrar.apply(
      Owned<Operation>(new RemoveSlave(slave)));

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

  AWAIT_TRUE(registrar.apply(Owned<Operation>(new AdmitSlave(slave))));
  AWAIT_FALSE(registrar.apply(Owned<Operation>(new AdmitSlave(slave))));
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

  AWAIT_TRUE(registrar.apply(Owned<Operation>(new AdmitSlave(info1))));

  // Attempting to mark a slave as reachable that is already reachable
  // should not result in an error.
  AWAIT_TRUE(registrar.apply(Owned<Operation>(new MarkSlaveReachable(info1))));
  AWAIT_TRUE(registrar.apply(Owned<Operation>(new MarkSlaveReachable(info1))));

  // Attempting to mark a slave as reachable that is not in the
  // unreachable list should not result in error.
  AWAIT_TRUE(registrar.apply(Owned<Operation>(new MarkSlaveReachable(info2))));
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

  AWAIT_TRUE(registrar.apply(Owned<Operation>(new AdmitSlave(info1))));

  AWAIT_TRUE(
      registrar.apply(
          Owned<Operation>(
              new MarkSlaveUnreachable(info1, protobuf::getCurrentTime()))));

  AWAIT_TRUE(
      registrar.apply(Owned<Operation>(new MarkSlaveReachable(info1))));

  AWAIT_TRUE(
      registrar.apply(
          Owned<Operation>(
              new MarkSlaveUnreachable(info1, protobuf::getCurrentTime()))));

  // If a slave is already unreachable, trying to mark it unreachable
  // again should fail.
  AWAIT_FALSE(
      registrar.apply(
          Owned<Operation>(
              new MarkSlaveUnreachable(info1, protobuf::getCurrentTime()))));
}


TEST_F(RegistrarTest, PruneUnreachable)
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

  AWAIT_TRUE(registrar.apply(Owned<Operation>(new AdmitSlave(info1))));
  AWAIT_TRUE(registrar.apply(Owned<Operation>(new AdmitSlave(info2))));

  AWAIT_TRUE(
      registrar.apply(
          Owned<Operation>(
              new MarkSlaveUnreachable(info1, protobuf::getCurrentTime()))));

  AWAIT_TRUE(
      registrar.apply(
          Owned<Operation>(
              new MarkSlaveUnreachable(info2, protobuf::getCurrentTime()))));

  AWAIT_TRUE(registrar.apply(Owned<Operation>(new PruneUnreachable({id1}))));
  AWAIT_TRUE(registrar.apply(Owned<Operation>(new PruneUnreachable({id2}))));

  AWAIT_TRUE(registrar.apply(Owned<Operation>(new AdmitSlave(info1))));

  AWAIT_TRUE(
      registrar.apply(
          Owned<Operation>(
              new MarkSlaveUnreachable(info1, protobuf::getCurrentTime()))));

  AWAIT_TRUE(registrar.apply(Owned<Operation>(new PruneUnreachable({id1}))));
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

  AWAIT_TRUE(registrar.apply(Owned<Operation>(new AdmitSlave(info1))));
  AWAIT_TRUE(registrar.apply(Owned<Operation>(new AdmitSlave(info2))));
  AWAIT_TRUE(registrar.apply(Owned<Operation>(new AdmitSlave(info3))));

  AWAIT_TRUE(registrar.apply(Owned<Operation>(new RemoveSlave(info1))));

  AWAIT_FALSE(registrar.apply(Owned<Operation>(new RemoveSlave(info1))));

  AWAIT_TRUE(registrar.apply(Owned<Operation>(new AdmitSlave(info1))));

  AWAIT_TRUE(registrar.apply(Owned<Operation>(new RemoveSlave(info2))));

  AWAIT_FALSE(registrar.apply(Owned<Operation>(new RemoveSlave(info2))));

  AWAIT_TRUE(registrar.apply(Owned<Operation>(new RemoveSlave(info3))));

  AWAIT_FALSE(registrar.apply(Owned<Operation>(new RemoveSlave(info3))));
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

    AWAIT_READY(registrar.apply(
        Owned<Operation>(new UpdateSchedule(schedule))));
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

    AWAIT_READY(registrar.apply(
        Owned<Operation>(new UpdateSchedule(schedule))));
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

    AWAIT_READY(registrar.apply(
        Owned<Operation>(new UpdateSchedule(schedule))));
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

    AWAIT_READY(registrar.apply(
        Owned<Operation>(new UpdateSchedule(schedule))));
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

    AWAIT_READY(registrar.apply(
        Owned<Operation>(new UpdateSchedule(schedule))));
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
    AWAIT_READY(registrar.apply(
        Owned<Operation>(new UpdateSchedule(schedule))));
  }

  {
    // Check that all statuses are removed.
    Registrar registrar(flags, state);
    Future<Registry> registry = registrar.recover(master);
    AWAIT_READY(registry);

    EXPECT_EQ(1, registry->schedules().size());
    EXPECT_EQ(0, registry->schedules(0).windows().size());
    EXPECT_EQ(0, registry->machines().machines().size());
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

    AWAIT_READY(registrar.apply(
        Owned<Operation>(new UpdateSchedule(schedule))));

    // Transition machine two into `DOWN` mode.
    RepeatedPtrField<MachineID> machines = createMachineList({machine2});

    AWAIT_READY(registrar.apply(
        Owned<Operation>(new StartMaintenance(machines))));
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

    AWAIT_READY(registrar.apply(
        Owned<Operation>(new UpdateSchedule(schedule))));

    // Deactivate the two `DRAINING` machines.
    RepeatedPtrField<MachineID> machines =
      createMachineList({machine1, machine3});

    AWAIT_READY(registrar.apply(
        Owned<Operation>(new StartMaintenance(machines))));
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

    AWAIT_READY(registrar.apply(
        Owned<Operation>(new UpdateSchedule(schedule))));

    // Transition machine three into `DOWN` mode.
    RepeatedPtrField<MachineID> machines = createMachineList({machine3});

    AWAIT_READY(registrar.apply(
        Owned<Operation>(new StartMaintenance(machines))));

    // Transition machine three into `UP` mode.
    AWAIT_READY(registrar.apply(
        Owned<Operation>(new StopMaintenance(machines))));
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

    AWAIT_READY(registrar.apply(
        Owned<Operation>(new StartMaintenance(machines))));

    // Transition all machines into `UP` mode.
    AWAIT_READY(registrar.apply(
        Owned<Operation>(new StopMaintenance(machines))));
  }

  {
    // Check that the schedule is now empty.
    Registrar registrar(flags, state);
    Future<Registry> registry = registrar.recover(master);
    AWAIT_READY(registry);

    EXPECT_EQ(0, registry->schedules().size());
    EXPECT_EQ(0, registry->machines().machines().size());
  }
}


// Tests that adding and updating quotas in the registry works properly.
TEST_F(RegistrarTest, UpdateQuota)
{
  const string ROLE1 = "role1";
  const string ROLE2 = "role2";

  // NOTE: `quotaResources1` yields a collection with two `Resource`
  // objects once converted to `RepeatedPtrField`.
  Resources quotaResources1 = Resources::parse("cpus:1;mem:1024").get();
  Resources quotaResources2 = Resources::parse("cpus:2").get();

  // Prepare `QuotaInfo` protobufs used in the test.
  QuotaInfo quota1;
  quota1.set_role(ROLE1);
  quota1.mutable_guarantee()->CopyFrom(quotaResources1);

  Option<Error> validateError1 = quota::validation::quotaInfo(quota1);
  EXPECT_NONE(validateError1);

  QuotaInfo quota2;
  quota2.set_role(ROLE2);
  quota2.mutable_guarantee()->CopyFrom(quotaResources1);

  Option<Error> validateError2 = quota::validation::quotaInfo(quota2);
  EXPECT_NONE(validateError2);

  {
    // Prepare the registrar; see the comment above why we need to do this in
    // every scope.
    Registrar registrar(flags, state);
    Future<Registry> registry = registrar.recover(master);
    AWAIT_READY(registry);

    // Store quota for a role without quota.
    AWAIT_TRUE(registrar.apply(Owned<Operation>(new UpdateQuota(quota1))));
  }

  {
    Registrar registrar(flags, state);
    Future<Registry> registry = registrar.recover(master);
    AWAIT_READY(registry);

    // Check that the recovered quota matches the one we stored.
    ASSERT_EQ(1, registry->quotas().size());
    EXPECT_EQ(ROLE1, registry->quotas(0).info().role());
    ASSERT_EQ(2, registry->quotas(0).info().guarantee().size());

    Resources storedResources(registry->quotas(0).info().guarantee());
    EXPECT_EQ(quotaResources1, storedResources);

    // Change quota for `ROLE1`.
    quota1.mutable_guarantee()->CopyFrom(quotaResources2);

    // Update the only stored quota.
    AWAIT_TRUE(registrar.apply(Owned<Operation>(new UpdateQuota(quota1))));
  }

  {
    Registrar registrar(flags, state);
    Future<Registry> registry = registrar.recover(master);
    AWAIT_READY(registry);

    // Check that the recovered quota matches the one we updated.
    ASSERT_EQ(1, registry->quotas().size());
    EXPECT_EQ(ROLE1, registry->quotas(0).info().role());
    ASSERT_EQ(1, registry->quotas(0).info().guarantee().size());

    Resources storedResources(registry->quotas(0).info().guarantee());
    EXPECT_EQ(quotaResources2, storedResources);

    // Store one more quota for a role without quota.
    AWAIT_TRUE(registrar.apply(Owned<Operation>(new UpdateQuota(quota2))));
  }

  {
    Registrar registrar(flags, state);
    Future<Registry> registry = registrar.recover(master);
    AWAIT_READY(registry);

    // Check that the recovered quotas match those we stored previously.
    // NOTE: We assume quota messages are stored in order they have
    // been added.
    // TODO(alexr): Consider removing dependency on the order.
    ASSERT_EQ(2, registry->quotas().size());
    EXPECT_EQ(ROLE1, registry->quotas(0).info().role());
    ASSERT_EQ(1, registry->quotas(0).info().guarantee().size());

    EXPECT_EQ(ROLE2, registry->quotas(1).info().role());
    ASSERT_EQ(2, registry->quotas(1).info().guarantee().size());

    Resources storedResources(registry->quotas(1).info().guarantee());
    EXPECT_EQ(quotaResources1, storedResources);

    // Change quota for `role2`.
    quota2.mutable_guarantee()->CopyFrom(quotaResources2);

    // Update quota for `role2` in presence of multiple quotas.
    AWAIT_TRUE(registrar.apply(Owned<Operation>(new UpdateQuota(quota2))));
  }

  {
    Registrar registrar(flags, state);
    Future<Registry> registry = registrar.recover(master);
    AWAIT_READY(registry);

    // Check that the recovered quotas match those we stored and updated
    // previously.
    // NOTE: We assume quota messages are stored in order they have been
    // added and update does not change the order.
    // TODO(alexr): Consider removing dependency on the order.
    ASSERT_EQ(2, registry->quotas().size());

    EXPECT_EQ(ROLE1, registry->quotas(0).info().role());
    ASSERT_EQ(1, registry->quotas(0).info().guarantee().size());

    Resources storedResources1(registry->quotas(0).info().guarantee());
    EXPECT_EQ(quotaResources2, storedResources1);

    EXPECT_EQ(ROLE2, registry->quotas(1).info().role());
    ASSERT_EQ(1, registry->quotas(1).info().guarantee().size());

    Resources storedResources2(registry->quotas(1).info().guarantee());
    EXPECT_EQ(quotaResources2, storedResources2);
  }
}


// Tests removing quotas from the registry.
TEST_F(RegistrarTest, RemoveQuota)
{
  const string ROLE1 = "role1";
  const string ROLE2 = "role2";

  {
    // Prepare the registrar; see the comment above why we need to do this in
    // every scope.
    Registrar registrar(flags, state);
    Future<Registry> registry = registrar.recover(master);
    AWAIT_READY(registry);

    // NOTE: `quotaResources` yields a collection with two `Resource`
    // objects once converted to `RepeatedPtrField`.
    Resources quotaResources1 = Resources::parse("cpus:1;mem:1024").get();
    Resources quotaResources2 = Resources::parse("cpus:2").get();

    // Prepare `QuotaInfo` protobufs.
    QuotaInfo quota1;
    quota1.set_role(ROLE1);
    quota1.mutable_guarantee()->CopyFrom(quotaResources1);

    Option<Error> validateError1 = quota::validation::quotaInfo(quota1);
    EXPECT_NONE(validateError1);

    QuotaInfo quota2;
    quota2.set_role(ROLE2);
    quota2.mutable_guarantee()->CopyFrom(quotaResources2);

    Option<Error> validateError2 = quota::validation::quotaInfo(quota2);
    EXPECT_NONE(validateError2);

    AWAIT_TRUE(registrar.apply(Owned<Operation>(new UpdateQuota(quota1))));
    AWAIT_TRUE(registrar.apply(Owned<Operation>(new UpdateQuota(quota2))));
  }

  {
    Registrar registrar(flags, state);
    Future<Registry> registry = registrar.recover(master);
    AWAIT_READY(registry);

    // Check that the recovered quotas match those we stored previously.
    // NOTE: We assume quota messages are stored in order they have been
    // added.
    // TODO(alexr): Consider removing dependency on the order.
    ASSERT_EQ(2, registry->quotas().size());
    EXPECT_EQ(ROLE1, registry->quotas(0).info().role());
    EXPECT_EQ(ROLE2, registry->quotas(1).info().role());

    // Remove quota for `role2`.
    AWAIT_TRUE(registrar.apply(Owned<Operation>(new RemoveQuota(ROLE2))));
  }

  {
    Registrar registrar(flags, state);
    Future<Registry> registry = registrar.recover(master);
    AWAIT_READY(registry);

    // Check that there is only one quota left in the registry.
    ASSERT_EQ(1, registry->quotas().size());
    EXPECT_EQ(ROLE1, registry->quotas(0).info().role());

    // Remove quota for `ROLE1`.
    AWAIT_TRUE(registrar.apply(Owned<Operation>(new RemoveQuota(ROLE1))));
  }

  {
    Registrar registrar(flags, state);
    Future<Registry> registry = registrar.recover(master);
    AWAIT_READY(registry);

    // Check that there are no more quotas at this point.
    ASSERT_EQ(0, registry->quotas().size());
  }
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

    ASSERT_EQ(0, registry->weights_size());

    // Store the weight for 'ROLE1' previously without weight.
    hashmap<string, double> weights;
    weights[ROLE1] = WEIGHT1;
    vector<WeightInfo> weightInfos = getWeightInfos(weights);

    AWAIT_TRUE(registrar.apply(
        Owned<Operation>(new UpdateWeights(weightInfos))));
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

    AWAIT_TRUE(registrar.apply(
        Owned<Operation>(new UpdateWeights(weightInfos))));
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

    AWAIT_TRUE(
        registrar.apply(Owned<Operation>(new MarkSlaveReachable(slave))));
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
  MOCK_METHOD2(set, Future<bool>(const Entry&, const UUID&));
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
  AWAIT_FAILED(registrar.apply(Owned<Operation>(new AdmitSlave(slave))));
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
  AWAIT_FAILED(registrar.apply(Owned<Operation>(new AdmitSlave(slave))));
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
  AWAIT_FAILED(registrar.apply(Owned<Operation>(new AdmitSlave(slave))));

  // The registrar should now be aborted!
  AWAIT_FAILED(registrar.apply(Owned<Operation>(new AdmitSlave(slave))));
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
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response)
    << response->body;

  Credential badCredential;
  badCredential.set_principal("bad-principal");
  badCredential.set_secret("bad-secret");

  // Requests with bad credentials should be rejected.
  response = process::http::get(
      registrar.pid(),
      "registry",
      None(),
      createBasicAuthHeaders(badCredential));
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response)
    << response->body;

  // Requests with good credentials should be permitted.
  response = process::http::get(
      registrar.pid(),
      "registry",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response)
    << response->body;

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
    result = registrar.apply(Owned<Operation>(new AdmitSlave(info)));
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
    result = registrar.apply(Owned<Operation>(new MarkSlaveReachable(info)));
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
    result = registrar2.apply(Owned<Operation>(new RemoveSlave(info)));
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
    result = registrar.apply(Owned<Operation>(new AdmitSlave(info)));
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
    result = registrar.apply(
        Owned<Operation>(new MarkSlaveUnreachable(info, unreachableTime)));
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
    result = registrar.apply(
        Owned<Operation>(new MarkSlaveReachable(info)));
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
    result = registrar.apply(Owned<Operation>(new AdmitSlave(info)));
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
    result = registrar.apply(
        Owned<Operation>(new MarkSlaveUnreachable(info, unreachableTime)));
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
  result = registrar.apply(Owned<Operation>(new PruneUnreachable(toRemove)));
  AWAIT_READY_FOR(result, Minutes(5));
  cout << "Garbage collected " << toRemove.size() << " agents in "
       << watch.elapsed() << endl;
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
