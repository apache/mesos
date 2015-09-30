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

#include <algorithm>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <mesos/attributes.hpp>
#include <mesos/type_utils.hpp>

#include <process/clock.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/pid.hpp>
#include <process/process.hpp>

#include <stout/bytes.hpp>
#include <stout/stopwatch.hpp>
#include <stout/uuid.hpp>

#include "common/protobuf_utils.hpp"

#include "log/log.hpp"
#include "log/replica.hpp"

#include "log/tool/initialize.hpp"

#include "messages/state.hpp"

#include "master/flags.hpp"
#include "master/maintenance.hpp"
#include "master/master.hpp"
#include "master/registrar.hpp"

#include "state/log.hpp"
#include "state/protobuf.hpp"
#include "state/storage.hpp"

#include "tests/utils.hpp"

using namespace mesos::internal::master;

using namespace process;

using mesos::internal::log::Log;
using mesos::internal::log::Replica;

using std::cout;
using std::endl;
using std::map;
using std::set;
using std::string;
using std::vector;

using process::Clock;

using google::protobuf::RepeatedPtrField;

using mesos::internal::protobuf::maintenance::createMachineList;
using mesos::internal::protobuf::maintenance::createSchedule;
using mesos::internal::protobuf::maintenance::createUnavailability;
using mesos::internal::protobuf::maintenance::createWindow;

using testing::_;
using testing::DoAll;
using testing::Eq;
using testing::Return;

using mesos::internal::tests::TemporaryDirectoryTest;

using ::testing::WithParamInterface;

namespace mesos {
namespace internal {
namespace tests {

using namespace mesos::maintenance;
using namespace mesos::internal::master::maintenance;

using state::Entry;
using state::LogStorage;
using state::Storage;

using state::protobuf::State;

// TODO(xujyan): This class copies code from LogStateTest. It would
// be nice to find a common location for log related base tests when
// there are more uses of it.
class RegistrarTestBase : public TemporaryDirectoryTest
{
public:
  RegistrarTestBase()
    : log(NULL),
      storage(NULL),
      state(NULL),
      replica2(NULL) {}

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


class RegistrarTest : public RegistrarTestBase,
                      public WithParamInterface<bool>
{
protected:
  virtual void SetUp()
  {
    RegistrarTestBase::SetUp();
    flags.registry_strict = GetParam();
  }
};


// The Registrar tests are parameterized by "strictness".
INSTANTIATE_TEST_CASE_P(Strict, RegistrarTest, ::testing::Bool());


TEST_P(RegistrarTest, Recover)
{
  Registrar registrar(flags, state);

  // Operations preceding recovery will fail.
  AWAIT_EXPECT_FAILED(
      registrar.apply(Owned<Operation>(new AdmitSlave(slave))));
  AWAIT_EXPECT_FAILED(
      registrar.apply(Owned<Operation>(new ReadmitSlave(slave))));
  AWAIT_EXPECT_FAILED(
      registrar.apply(Owned<Operation>(new RemoveSlave(slave))));

  Future<Registry> registry = registrar.recover(master);

  // Before waiting for the recovery to complete, invoke some
  // operations to ensure they do not fail.
  Future<bool> admit = registrar.apply(
      Owned<Operation>(new AdmitSlave(slave)));
  Future<bool> readmit = registrar.apply(
      Owned<Operation>(new ReadmitSlave(slave)));
  Future<bool> remove = registrar.apply(
      Owned<Operation>(new RemoveSlave(slave)));

  AWAIT_READY(registry);
  EXPECT_EQ(master, registry.get().master().info());

  AWAIT_EQ(true, admit);
  AWAIT_EQ(true, readmit);
  AWAIT_EQ(true, remove);
}


TEST_P(RegistrarTest, Admit)
{
  Registrar registrar(flags, state);
  AWAIT_READY(registrar.recover(master));

  AWAIT_EQ(true, registrar.apply(Owned<Operation>(new AdmitSlave(slave))));

  if (flags.registry_strict) {
    AWAIT_EQ(false, registrar.apply(Owned<Operation>(new AdmitSlave(slave))));
  } else {
    AWAIT_EQ(true, registrar.apply(Owned<Operation>(new AdmitSlave(slave))));
  }
}


TEST_P(RegistrarTest, Readmit)
{
  Registrar registrar(flags, state);
  AWAIT_READY(registrar.recover(master));

  SlaveInfo info1;
  info1.set_hostname("localhost");

  SlaveID id1;
  id1.set_value("1");
  info1.mutable_id()->CopyFrom(id1);

  SlaveID id2;
  id2.set_value("2");

  SlaveInfo info2;
  info2.set_hostname("localhost");
  info2.mutable_id()->CopyFrom(id2);

  AWAIT_EQ(true, registrar.apply(Owned<Operation>(new AdmitSlave(info1))));

  AWAIT_EQ(true, registrar.apply(Owned<Operation>(new ReadmitSlave(info1))));

  if (flags.registry_strict) {
    AWAIT_EQ(false, registrar.apply(Owned<Operation>(new ReadmitSlave(info2))));
  } else {
    AWAIT_EQ(true, registrar.apply(Owned<Operation>(new ReadmitSlave(info2))));
  }
}


TEST_P(RegistrarTest, Remove)
{
  Registrar registrar(flags, state);
  AWAIT_READY(registrar.recover(master));

  SlaveInfo info1;
  info1.set_hostname("localhost");

  SlaveID id1;
  id1.set_value("1");
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

  AWAIT_EQ(true, registrar.apply(Owned<Operation>(new AdmitSlave(info1))));
  AWAIT_EQ(true, registrar.apply(Owned<Operation>(new AdmitSlave(info2))));
  AWAIT_EQ(true, registrar.apply(Owned<Operation>(new AdmitSlave(info3))));

  AWAIT_EQ(true, registrar.apply(Owned<Operation>(new RemoveSlave(info1))));

  if (flags.registry_strict) {
    AWAIT_EQ(false, registrar.apply(Owned<Operation>(new RemoveSlave(info1))));
  } else {
    AWAIT_EQ(true, registrar.apply(Owned<Operation>(new RemoveSlave(info1))));
  }

  AWAIT_EQ(true, registrar.apply(Owned<Operation>(new AdmitSlave(info1))));

  AWAIT_EQ(true, registrar.apply(Owned<Operation>(new RemoveSlave(info2))));

  if (flags.registry_strict) {
    AWAIT_EQ(false, registrar.apply(Owned<Operation>(new RemoveSlave(info2))));
  } else {
    AWAIT_EQ(true, registrar.apply(Owned<Operation>(new RemoveSlave(info2))));
  }

  AWAIT_EQ(true, registrar.apply(Owned<Operation>(new RemoveSlave(info3))));

  if (flags.registry_strict) {
    AWAIT_EQ(false, registrar.apply(Owned<Operation>(new RemoveSlave(info3))));
  } else {
    AWAIT_EQ(true, registrar.apply(Owned<Operation>(new RemoveSlave(info3))));
  }
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
//     EXPECT_EQ(1, registry.get().schedules().size());
//   });

// Adds maintenance schedules to the registry, one machine at a time.
// Then removes machines from the schedule.
TEST_P(RegistrarTest, UpdateMaintenanceSchedule)
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

    EXPECT_EQ(1, registry.get().schedules().size());
    EXPECT_EQ(1, registry.get().schedules(0).windows().size());
    EXPECT_EQ(1, registry.get().schedules(0).windows(0).machine_ids().size());
    EXPECT_EQ(1, registry.get().machines().machines().size());
    EXPECT_EQ(
        MachineInfo::DRAINING,
        registry.get().machines().machines(0).info().mode());

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

    EXPECT_EQ(1, registry.get().schedules().size());
    EXPECT_EQ(2, registry.get().schedules(0).windows().size());
    EXPECT_EQ(1, registry.get().schedules(0).windows(0).machine_ids().size());
    EXPECT_EQ(1, registry.get().schedules(0).windows(1).machine_ids().size());
    EXPECT_EQ(2, registry.get().machines().machines().size());
    EXPECT_EQ(
        MachineInfo::DRAINING,
        registry.get().machines().machines(1).info().mode());

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

    EXPECT_EQ(1, registry.get().schedules().size());
    EXPECT_EQ(2, registry.get().schedules(0).windows().size());
    EXPECT_EQ(1, registry.get().schedules(0).windows(0).machine_ids().size());
    EXPECT_EQ(2, registry.get().schedules(0).windows(1).machine_ids().size());
    EXPECT_EQ(3, registry.get().machines().machines().size());
    EXPECT_EQ(
        MachineInfo::DRAINING,
        registry.get().machines().machines(2).info().mode());

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

    EXPECT_EQ(1, registry.get().schedules().size());
    EXPECT_EQ(1, registry.get().schedules(0).windows().size());
    EXPECT_EQ(3, registry.get().schedules(0).windows(0).machine_ids().size());
    EXPECT_EQ(3, registry.get().machines().machines().size());
    EXPECT_EQ(
        MachineInfo::DRAINING,
        registry.get().machines().machines(0).info().mode());

    EXPECT_EQ(
        MachineInfo::DRAINING,
        registry.get().machines().machines(1).info().mode());

    EXPECT_EQ(
        MachineInfo::DRAINING,
        registry.get().machines().machines(2).info().mode());

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

    EXPECT_EQ(1, registry.get().schedules().size());
    EXPECT_EQ(1, registry.get().schedules(0).windows().size());
    EXPECT_EQ(2, registry.get().schedules(0).windows(0).machine_ids().size());
    EXPECT_EQ(2, registry.get().machines().machines().size());

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

    EXPECT_EQ(1, registry.get().schedules().size());
    EXPECT_EQ(0, registry.get().schedules(0).windows().size());
    EXPECT_EQ(0, registry.get().machines().machines().size());
  }
}


// Creates a schedule and properly starts maintenance.
TEST_P(RegistrarTest, StartMaintenance)
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

    EXPECT_EQ(2, registry.get().machines().machines().size());
    EXPECT_EQ(
        MachineInfo::DRAINING,
        registry.get().machines().machines(0).info().mode());

    EXPECT_EQ(
        MachineInfo::DOWN,
        registry.get().machines().machines(1).info().mode());

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

    EXPECT_EQ(3, registry.get().machines().machines().size());
    EXPECT_EQ(
        MachineInfo::DOWN,
        registry.get().machines().machines(0).info().mode());

    EXPECT_EQ(
        MachineInfo::DOWN,
        registry.get().machines().machines(1).info().mode());

    EXPECT_EQ(
        MachineInfo::DOWN,
        registry.get().machines().machines(2).info().mode());
  }
}


// Creates a schedule and properly starts and stops maintenance.
TEST_P(RegistrarTest, StopMaintenance)
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

    EXPECT_EQ(1, registry.get().schedules().size());
    EXPECT_EQ(1, registry.get().schedules(0).windows().size());
    EXPECT_EQ(2, registry.get().schedules(0).windows(0).machine_ids().size());
    EXPECT_EQ(2, registry.get().machines().machines().size());
    EXPECT_EQ(
        MachineInfo::DRAINING,
        registry.get().machines().machines(0).info().mode());

    EXPECT_EQ(
        MachineInfo::DRAINING,
        registry.get().machines().machines(1).info().mode());

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

    EXPECT_EQ(0, registry.get().schedules().size());
    EXPECT_EQ(0, registry.get().machines().machines().size());
  }
}


TEST_P(RegistrarTest, Bootstrap)
{
  // Run 1 readmits a slave that is not present.
  {
    Registrar registrar(flags, state);
    AWAIT_READY(registrar.recover(master));

    // If not strict, we should be allowed to readmit the slave.
    if (flags.registry_strict) {
      AWAIT_EQ(false,
               registrar.apply(Owned<Operation>(new ReadmitSlave(slave))));
    } else {
      AWAIT_EQ(true,
               registrar.apply(Owned<Operation>(new ReadmitSlave(slave))));
    }
  }

  // Run 2 should see the slave if not strict.
  {
    Registrar registrar(flags, state);

    Future<Registry> registry = registrar.recover(master);

    AWAIT_READY(registry);

    if (flags.registry_strict) {
      EXPECT_EQ(0, registry.get().slaves().slaves().size());
    } else {
      ASSERT_EQ(1, registry.get().slaves().slaves().size());
      EXPECT_EQ(slave, registry.get().slaves().slaves(0).info());
    }
  }
}


class MockStorage : public Storage
{
public:
  MOCK_METHOD1(get, Future<Option<Entry> >(const string&));
  MOCK_METHOD2(set, Future<bool>(const Entry&, const UUID&));
  MOCK_METHOD1(expunge, Future<bool>(const Entry&));
  MOCK_METHOD0(names, Future<std::set<string> >(void));
};


TEST_P(RegistrarTest, FetchTimeout)
{
  Clock::pause();

  MockStorage storage;
  State state(&storage);

  Future<Nothing> get;
  EXPECT_CALL(storage, get(_))
    .WillOnce(DoAll(FutureSatisfy(&get),
                    Return(Future<Option<Entry> >())));

  Registrar registrar(flags, &state);

  Future<Registry> recover = registrar.recover(master);

  AWAIT_READY(get);

  Clock::advance(flags.registry_fetch_timeout);

  AWAIT_FAILED(recover);

  Clock::resume();

  // Ensure the registrar fails subsequent operations.
  AWAIT_FAILED(registrar.apply(Owned<Operation>(new AdmitSlave(slave))));
}


TEST_P(RegistrarTest, StoreTimeout)
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


TEST_P(RegistrarTest, Abort)
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


class Registrar_BENCHMARK_Test : public RegistrarTestBase,
                                 public WithParamInterface<size_t>
{};


// The Registrar benchmark tests are parameterized by the number of slaves.
INSTANTIATE_TEST_CASE_P(
    SlaveCount,
    Registrar_BENCHMARK_Test,
    ::testing::Values(10000U, 20000U, 30000U, 50000U));


TEST_P(Registrar_BENCHMARK_Test, Performance)
{
  Registrar registrar(flags, state);
  AWAIT_READY(registrar.recover(master));

  vector<SlaveInfo> infos;

  Attributes attributes = Attributes::parse("foo:bar;baz:quux");
  Resources resources =
    Resources::parse("cpus(*):1.0;mem(*):512;disk(*):2048").get();

  size_t slaveCount = GetParam();

  // Create slaves.
  for (size_t i = 0; i < slaveCount; ++i) {
    // Simulate real slave information.
    SlaveInfo info;
    info.set_hostname("localhost");
    info.mutable_id()->set_value(
        std::string("201310101658-2280333834-5050-48574-") + stringify(i));
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
  LOG(INFO) << "Admitted " << slaveCount << " slaves in " << watch.elapsed();

  // Shuffle the slaves so we are readmitting them in random order (
  // same as in production).
  std::random_shuffle(infos.begin(), infos.end());

  // Readmit slaves.
  watch.start();
  foreach (const SlaveInfo& info, infos) {
    result = registrar.apply(Owned<Operation>(new ReadmitSlave(info)));
  }
  AWAIT_READY_FOR(result, Minutes(5));
  LOG(INFO) << "Readmitted " << slaveCount << " slaves in " << watch.elapsed();

  // Recover slaves.
  Registrar registrar2(flags, state);
  watch.start();
  MasterInfo info;
  info.set_id("master");
  info.set_ip(10000000);
  info.set_port(5050);
  Future<Registry> registry = registrar2.recover(info);
  AWAIT_READY(registry);
  LOG(INFO) << "Recovered " << slaveCount << " slaves ("
            << Bytes(registry.get().ByteSize()) << ") in " << watch.elapsed();

  // Shuffle the slaves so we are removing them in random order (same
  // as in production).
  std::random_shuffle(infos.begin(), infos.end());

  // Remove slaves.
  watch.start();
  foreach (const SlaveInfo& info, infos) {
    result = registrar2.apply(Owned<Operation>(new RemoveSlave(info)));
  }
  AWAIT_READY_FOR(result, Minutes(5));
  cout << "Removed " << slaveCount << " slaves in " << watch.elapsed() << endl;
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
