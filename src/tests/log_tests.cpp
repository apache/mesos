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

#include <stdint.h>

#include <list>
#include <set>
#include <string>

#include <gmock/gmock.h>

#include <mesos/log/log.hpp>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>
#include <process/shared.hpp>

#include <stout/gtest.hpp>
#include <stout/none.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/stopwatch.hpp>
#include <stout/try.hpp>

#include <stout/tests/utils.hpp>

#include "log/catchup.hpp"
#include "log/coordinator.hpp"
#include "log/leveldb.hpp"
#include "log/network.hpp"
#include "log/storage.hpp"
#include "log/recover.hpp"
#include "log/replica.hpp"
#include "log/tool/initialize.hpp"

#include "tests/environment.hpp"
#include "tests/mesos.hpp"
#include "tests/utils.hpp"

#ifdef MESOS_HAS_JAVA
#include "tests/zookeeper.hpp"
#endif

using namespace mesos::internal::log;

using namespace process;

using std::list;
using std::set;
using std::string;

using testing::_;
using testing::Eq;
using testing::Invoke;
using testing::Return;

using mesos::log::Log;

namespace mesos {
namespace internal {
namespace tests {


TEST(NetworkTest, Watch)
{
  UPID pid1 = ProcessBase().self();
  UPID pid2 = ProcessBase().self();

  Network network;

  // Test the default parameter.
  Future<size_t> future = network.watch(1u);
  AWAIT_READY(future);
  EXPECT_EQ(0u, future.get());

  future = network.watch(2u, Network::NOT_EQUAL_TO);
  AWAIT_READY(future);
  EXPECT_EQ(0u, future.get());

  future = network.watch(0u, Network::GREATER_THAN_OR_EQUAL_TO);
  AWAIT_READY(future);
  EXPECT_EQ(0u, future.get());

  future = network.watch(1u, Network::LESS_THAN);
  AWAIT_READY(future);
  EXPECT_EQ(0u, future.get());

  network.add(pid1);

  future = network.watch(1u, Network::EQUAL_TO);
  AWAIT_READY(future);
  EXPECT_EQ(1u, future.get());

  future = network.watch(1u, Network::GREATER_THAN);
  ASSERT_TRUE(future.isPending());

  network.add(pid2);

  AWAIT_READY(future);
  EXPECT_EQ(2u, future.get());

  future = network.watch(1u, Network::LESS_THAN_OR_EQUAL_TO);
  ASSERT_TRUE(future.isPending());

  network.remove(pid2);

  AWAIT_READY(future);
  EXPECT_EQ(1u, future.get());
}


template <typename T>
class LogStorageTest : public TemporaryDirectoryTest {};


typedef ::testing::Types<LevelDBStorage> LogStorageTypes;


TYPED_TEST_CASE(LogStorageTest, LogStorageTypes);


TYPED_TEST(LogStorageTest, Truncate)
{
  TypeParam storage;

  Try<Storage::State> state = storage.restore(os::getcwd() + "/.log");
  ASSERT_SOME(state);

  EXPECT_EQ(Metadata::EMPTY, state->metadata.status());
  EXPECT_EQ(0u, state->metadata.promised());
  EXPECT_EQ(0u, state->begin);
  EXPECT_EQ(0u, state->end);

  // Append from position 0 to position 9.
  for (uint64_t i = 0; i < 10; i++) {
    Action action;
    action.set_position(i);
    action.set_promised(1);
    action.set_performed(1);
    action.set_learned(true);
    action.set_type(Action::APPEND);
    action.mutable_append()->set_bytes(stringify(i));

    ASSERT_SOME(storage.persist(action));
  }

  for (uint64_t i = 0; i < 10; i++) {
    Try<Action> action = storage.read(i);
    ASSERT_SOME(action);

    EXPECT_EQ(i, action->position());
    EXPECT_EQ(1u, action->promised());
    EXPECT_EQ(1u, action->performed());
    EXPECT_TRUE(action->learned());
    EXPECT_EQ(Action::APPEND, action->type());
    ASSERT_TRUE(action->has_append());
    EXPECT_EQ(stringify(i), action->append().bytes());
  }

  // Truncate to position 3 (at position 10).
  Action truncate;
  truncate.set_position(10);
  truncate.set_promised(1);
  truncate.set_performed(1);
  truncate.set_learned(true);
  truncate.set_type(Action::TRUNCATE);
  truncate.mutable_truncate()->set_to(3);

  ASSERT_SOME(storage.persist(truncate));

  for (uint64_t i = 0; i < 11; i++) {
    Try<Action> action = storage.read(i);

    if (i < 3) {
      // Position 0, 1 and 2 have been truncated.
      EXPECT_ERROR(action);
    } else if (i == 10) {
      // Position 10 is a truncate.
      EXPECT_EQ(i, action->position());
      EXPECT_EQ(1u, action->promised());
      EXPECT_EQ(1u, action->performed());
      EXPECT_TRUE(action->learned());
      EXPECT_EQ(Action::TRUNCATE, action->type());
      ASSERT_TRUE(action->has_truncate());
      EXPECT_EQ(3u, action->truncate().to());
    } else {
      EXPECT_EQ(i, action->position());
      EXPECT_EQ(1u, action->promised());
      EXPECT_EQ(1u, action->performed());
      EXPECT_TRUE(action->learned());
      EXPECT_EQ(Action::APPEND, action->type());
      ASSERT_TRUE(action->has_append());
      EXPECT_EQ(stringify(i), action->append().bytes());
    }
  }

  // Truncate to position 10 (at position 11).
  truncate.set_position(11);
  truncate.set_promised(1);
  truncate.set_performed(1);
  truncate.set_learned(true);
  truncate.set_type(Action::TRUNCATE);
  truncate.mutable_truncate()->set_to(10);

  ASSERT_SOME(storage.persist(truncate));

  for (uint64_t i = 0; i < 12; i++) {
    Try<Action> action = storage.read(i);

    if (i < 10) {
      // Position 0 to 9 have been truncated.
      EXPECT_ERROR(action);
    } else if (i == 10) {
      // Position 10 is a truncate (to position 3).
      EXPECT_EQ(i, action->position());
      EXPECT_EQ(1u, action->promised());
      EXPECT_EQ(1u, action->performed());
      EXPECT_TRUE(action->learned());
      EXPECT_EQ(Action::TRUNCATE, action->type());
      ASSERT_TRUE(action->has_truncate());
      EXPECT_EQ(3u, action->truncate().to());
    } else if (i == 11) {
      // Position 11 is a truncate (to position 10).
      EXPECT_EQ(i, action->position());
      EXPECT_EQ(1u, action->promised());
      EXPECT_EQ(1u, action->performed());
      EXPECT_TRUE(action->learned());
      EXPECT_EQ(Action::TRUNCATE, action->type());
      ASSERT_TRUE(action->has_truncate());
      EXPECT_EQ(10u, action->truncate().to());
    }
  }
}


TYPED_TEST(LogStorageTest, TruncateWithEmptyLog)
{
  TypeParam storage;

  Try<Storage::State> state = storage.restore(os::getcwd() + "/.log");
  ASSERT_SOME(state);

  Action truncate;
  truncate.set_position(1);
  truncate.set_promised(1);
  truncate.set_performed(1);
  truncate.set_learned(true);
  truncate.set_type(Action::TRUNCATE);
  truncate.mutable_truncate()->set_to(0);

  ASSERT_SOME(storage.persist(truncate));

  Try<Action> action0 = storage.read(0);
  EXPECT_ERROR(action0);

  Try<Action> action1 = storage.read(1);
  EXPECT_EQ(1u, action1->position());
  EXPECT_EQ(1u, action1->promised());
  EXPECT_EQ(1u, action1->performed());
  EXPECT_TRUE(action1->learned());
  EXPECT_EQ(Action::TRUNCATE, action1->type());
  ASSERT_TRUE(action1->has_truncate());
  EXPECT_EQ(0u, action1->truncate().to());
}


TYPED_TEST(LogStorageTest, TruncateWithManyHoles)
{
  TypeParam storage;

  Try<Storage::State> state = storage.restore(os::getcwd() + "/.log");
  ASSERT_SOME(state);

  Action truncate;
  truncate.set_position(600020000);
  truncate.set_promised(1);
  truncate.set_performed(1);
  truncate.set_learned(true);
  truncate.set_type(Action::TRUNCATE);
  truncate.mutable_truncate()->set_to(600000000);

  // Measure the time taken for the truncation.
  Stopwatch stopwatch;
  stopwatch.start();

  ASSERT_SOME(storage.persist(truncate));

  // This truncation should not take much time because no position is
  // actually being truncated.
  EXPECT_GT(Seconds(1), stopwatch.elapsed());

  Try<Action> action = storage.read(600020000);

  EXPECT_EQ(600020000u, action->position());
  EXPECT_EQ(1u, action->promised());
  EXPECT_EQ(1u, action->performed());
  EXPECT_TRUE(action->learned());
  EXPECT_EQ(Action::TRUNCATE, action->type());
  ASSERT_TRUE(action->has_truncate());
  EXPECT_EQ(600000000u, action->truncate().to());
}


TYPED_TEST(LogStorageTest, Overflow)
{
  // This test checks that the log storage correctly handles positions
  // up to UINT64_MAX, see MESOS-10216.
  // Specifically we check some values which would overflow when `encode`
  // formatted them as signed integer, and collide with each other.
  TypeParam storage;

  Try<Storage::State> state = storage.restore(os::getcwd() + "/.log");
  ASSERT_SOME(state);

  EXPECT_EQ(Metadata::EMPTY, state->metadata.status());
  EXPECT_EQ(0u, state->metadata.promised());
  EXPECT_EQ(0u, state->begin);
  EXPECT_EQ(0u, state->end);

  // Note that position 0 is reserved so internally the positions below
  // have 1 added to them before being encoded, see `encode`.
  std::vector<uint64_t> positions{
                          // The positions below are encoded using legacy
                          // 10-width format.
    0,
    INT32_MAX - 1,
    INT32_MAX,            // Would overflow to INT32_MIN.
    2ull * INT32_MAX + 1, // Would overflow to 0.
    4ull * INT32_MAX + 3, // Would overflow to 0 as well.
    UINT32_MAX - 1,
    UINT32_MAX,
    9999999998,           // 1 below legacy 10-width limit.

                          // The positions below are encoded with new A+20-width
                          // format.
    9999999999,
    10000000000,
    9999999999 + 2 * (INT32_MAX + 1ull),
    10000000000 + 2 * (INT32_MAX + 1ull),
    UINT64_MAX - 1 - 2 * (INT32_MAX + 1ull),
    UINT64_MAX - 1,
  };

  foreach(uint64_t i, positions) {
    Action action;
    action.set_position(i);
    action.set_promised(1);
    action.set_performed(1);
    action.set_learned(true);
    action.set_type(Action::APPEND);
    action.mutable_append()->set_bytes(stringify(i));

    ASSERT_SOME(storage.persist(action));
  }

  foreach(uint64_t i, positions) {
    Try<Action> action = storage.read(i);
    ASSERT_SOME(action);

    EXPECT_EQ(i, action->position());
    EXPECT_EQ(1u, action->promised());
    EXPECT_EQ(1u, action->performed());
    EXPECT_TRUE(action->learned());
    EXPECT_EQ(Action::APPEND, action->type());
    ASSERT_TRUE(action->has_append());
    EXPECT_EQ(stringify(i), action->append().bytes());
  }
}


TYPED_TEST(LogStorageTest, OverflowTruncate)
{
  // Similar to test `Overflow` above, but here we test `truncate`
  // which checks that the encoded positions are strictly monotonically
  // increasing.
  // The reason we check this in a separate test is that `truncate`
  // requires iterating over positions from the first known position
  // in the log, so the `Overflow` test above would take too long
  // since it starts from 0.
  TypeParam storage;

  Try<Storage::State> state = storage.restore(os::getcwd() + "/.log");
  ASSERT_SOME(state);

  EXPECT_EQ(Metadata::EMPTY, state->metadata.status());
  EXPECT_EQ(0u, state->metadata.promised());
  EXPECT_EQ(0u, state->begin);
  EXPECT_EQ(0u, state->end);

  // Test positions around the legacy 10-width limit.
  std::vector<uint64_t> positions{
    9999999997,
    9999999998,
    9999999999,
    10000000000,
    10000000042,
  };

  foreach(uint64_t i, positions) {
    Action action;
    action.set_position(i);
    action.set_promised(1);
    action.set_performed(1);
    action.set_learned(true);
    action.set_type(Action::APPEND);
    action.mutable_append()->set_bytes(stringify(i));

    ASSERT_SOME(storage.persist(action));
  }

  // Truncate to position 9'999'999'999 (at position 10'000'000'000).
  Action truncate;
  truncate.set_position(10000000000ull);
  truncate.set_promised(1);
  truncate.set_performed(1);
  truncate.set_learned(true);
  truncate.set_type(Action::TRUNCATE);
  truncate.mutable_truncate()->set_to(9999999999ull);

  ASSERT_SOME(storage.persist(truncate));

  foreach(uint64_t i, positions) {
    Try<Action> action = storage.read(i);

    if (i < 9999999999ull) {
      // Position below 9'999'999'999 have been truncated.
      EXPECT_ERROR(action);
    } else if (i == 10000000000ull) {
      // Position 10'000'000'000 is a truncate (to position
      // 9'999'999'999).
      EXPECT_EQ(i, action->position());
      EXPECT_EQ(1u, action->promised());
      EXPECT_EQ(1u, action->performed());
      EXPECT_TRUE(action->learned());
      EXPECT_EQ(Action::TRUNCATE, action->type());
      ASSERT_TRUE(action->has_truncate());
      EXPECT_EQ(9999999999ull, action->truncate().to());
    } else {
      // Remaining positions have been preserved.
      ASSERT_SOME(action);
      EXPECT_EQ(i, action->position());
      EXPECT_EQ(1u, action->promised());
      EXPECT_EQ(1u, action->performed());
      EXPECT_TRUE(action->learned());
      EXPECT_EQ(Action::APPEND, action->type());
      ASSERT_TRUE(action->has_append());
      EXPECT_EQ(stringify(i), action->append().bytes());
    }
  }
}

class ReplicaTest : public TemporaryDirectoryTest
{
protected:
  // Used to change the status of a replicated log from `EMPTY` to `VOTING`.
  tool::Initialize initializer;
};


TEST_F(ReplicaTest, Promise)
{
  const string path = os::getcwd() + "/.log";
  initializer.flags.path = path;
  ASSERT_SOME(initializer.execute());

  Replica replica(path);

  PromiseRequest request;
  request.set_proposal(2);

  Future<PromiseResponse> response =
    protocol::promise(replica.pid(), request);

  AWAIT_READY(response);

  EXPECT_EQ(PromiseResponse::ACCEPT, response->type());
  EXPECT_TRUE(response->okay());
  EXPECT_EQ(2u, response->proposal());
  EXPECT_TRUE(response->has_position());
  EXPECT_EQ(0u, response->position());
  EXPECT_FALSE(response->has_action());

  request.set_proposal(1);

  response = protocol::promise(replica.pid(), request);

  AWAIT_READY(response);

  EXPECT_EQ(PromiseResponse::REJECT, response->type());
  EXPECT_FALSE(response->okay());
  EXPECT_EQ(2u, response->proposal()); // Highest proposal seen so far.
  EXPECT_FALSE(response->has_position());
  EXPECT_FALSE(response->has_action());

  request.set_proposal(3);

  response = protocol::promise(replica.pid(), request);

  AWAIT_READY(response);

  EXPECT_EQ(PromiseResponse::ACCEPT, response->type());
  EXPECT_TRUE(response->okay());
  EXPECT_EQ(3u, response->proposal());
  EXPECT_TRUE(response->has_position());
  EXPECT_EQ(0u, response->position());
  EXPECT_FALSE(response->has_action());
}


TEST_F(ReplicaTest, Append)
{
  const string path = os::getcwd() + "/.log";
  initializer.flags.path = path;
  ASSERT_SOME(initializer.execute());

  Replica replica(path);

  const uint64_t proposal = 1;

  PromiseRequest request1;
  request1.set_proposal(proposal);

  Future<PromiseResponse> response1 =
    protocol::promise(replica.pid(), request1);

  AWAIT_READY(response1);

  EXPECT_EQ(PromiseResponse::ACCEPT, response1->type());
  EXPECT_TRUE(response1->okay());
  EXPECT_EQ(proposal, response1->proposal());
  EXPECT_TRUE(response1->has_position());
  EXPECT_EQ(0u, response1->position());
  EXPECT_FALSE(response1->has_action());

  WriteRequest request2;
  request2.set_proposal(proposal);
  request2.set_position(1);
  request2.set_type(Action::APPEND);
  request2.mutable_append()->set_bytes("hello world");

  Future<WriteResponse> response2 =
    protocol::write(replica.pid(), request2);

  AWAIT_READY(response2);

  EXPECT_EQ(WriteResponse::ACCEPT, response2->type());
  EXPECT_TRUE(response2->okay());
  EXPECT_EQ(proposal, response2->proposal());
  EXPECT_EQ(1u, response2->position());

  Future<list<Action>> actions = replica.read(1, 1);

  AWAIT_READY(actions);
  ASSERT_EQ(1u, actions->size());

  Action action = actions->front();
  EXPECT_EQ(1u, action.position());
  EXPECT_EQ(1u, action.promised());
  EXPECT_TRUE(action.has_performed());
  EXPECT_EQ(1u, action.performed());
  EXPECT_FALSE(action.has_learned());
  EXPECT_TRUE(action.has_type());
  EXPECT_EQ(Action::APPEND, action.type());
  EXPECT_FALSE(action.has_nop());
  EXPECT_TRUE(action.has_append());
  EXPECT_FALSE(action.has_truncate());
  EXPECT_EQ("hello world", action.append().bytes());
}


TEST_F(ReplicaTest, Restore)
{
  const string path = os::getcwd() + "/.log";
  initializer.flags.path = path;
  ASSERT_SOME(initializer.execute());

  // By design only a single process can access leveldb at a time. In
  // this test, two instances of log replica need to open a connection
  // to the leveldb. By introducing scope levels we ensure that the first
  // instance is destructed and hence closes the connection before the
  // second instance opens it.
  {
    Replica replica1(path);

    const uint64_t proposal = 1;

    PromiseRequest request1;
    request1.set_proposal(proposal);

    Future<PromiseResponse> response1 =
      protocol::promise(replica1.pid(), request1);

    AWAIT_READY(response1);

    EXPECT_EQ(PromiseResponse::ACCEPT, response1->type());
    EXPECT_TRUE(response1->okay());
    EXPECT_EQ(proposal, response1->proposal());
    EXPECT_TRUE(response1->has_position());
    EXPECT_EQ(0u, response1->position());
    EXPECT_FALSE(response1->has_action());

    WriteRequest request2;
    request2.set_proposal(proposal);
    request2.set_position(1);
    request2.set_type(Action::APPEND);
    request2.mutable_append()->set_bytes("hello world");

    Future<WriteResponse> response2 =
      protocol::write(replica1.pid(), request2);

    AWAIT_READY(response2);

    EXPECT_EQ(WriteResponse::ACCEPT, response2->type());
    EXPECT_TRUE(response2->okay());
    EXPECT_EQ(proposal, response2->proposal());
    EXPECT_EQ(1u, response2->position());

    Future<list<Action>> actions1 = replica1.read(1, 1);

    AWAIT_READY(actions1);
    ASSERT_EQ(1u, actions1->size());

    {
      Action action = actions1->front();
      EXPECT_EQ(1u, action.position());
      EXPECT_EQ(1u, action.promised());
      EXPECT_TRUE(action.has_performed());
      EXPECT_EQ(1u, action.performed());
      EXPECT_FALSE(action.has_learned());
      EXPECT_TRUE(action.has_type());
      EXPECT_EQ(Action::APPEND, action.type());
      EXPECT_FALSE(action.has_nop());
      EXPECT_TRUE(action.has_append());
      EXPECT_FALSE(action.has_truncate());
      EXPECT_EQ("hello world", action.append().bytes());
    }
  }

  {
    Replica replica2(path);

    Future<list<Action>> actions2 = replica2.read(1, 1);

    AWAIT_READY(actions2);
    ASSERT_EQ(1u, actions2->size());

    {
      Action action = actions2->front();
      EXPECT_EQ(1u, action.position());
      EXPECT_EQ(1u, action.promised());
      EXPECT_TRUE(action.has_performed());
      EXPECT_EQ(1u, action.performed());
      EXPECT_FALSE(action.has_learned());
      EXPECT_TRUE(action.has_type());
      EXPECT_EQ(Action::APPEND, action.type());
      EXPECT_FALSE(action.has_nop());
      EXPECT_TRUE(action.has_append());
      EXPECT_FALSE(action.has_truncate());
      EXPECT_EQ("hello world", action.append().bytes());
    }
  }
}


// This test verifies that a non-VOTING replica replies to promise and
// write requests with an "ignored" response.
TEST_F(ReplicaTest, NonVoting)
{
  const string path = os::getcwd() + "/.log";

  Replica replica(path);

  PromiseRequest promiseRequest;
  promiseRequest.set_proposal(2);

  Future<PromiseResponse> promiseResponse =
    protocol::promise(replica.pid(), promiseRequest);

  AWAIT_READY(promiseResponse);

  EXPECT_EQ(PromiseResponse::IGNORED, promiseResponse->type());
  EXPECT_FALSE(promiseResponse->okay());
  EXPECT_EQ(2u, promiseResponse->proposal());

  WriteRequest writeRequest;
  writeRequest.set_proposal(3);
  writeRequest.set_position(1);
  writeRequest.set_type(Action::APPEND);
  writeRequest.mutable_append()->set_bytes("hello world");

  Future<WriteResponse> writeResponse =
    protocol::write(replica.pid(), writeRequest);

  AWAIT_READY(writeResponse);

  EXPECT_EQ(WriteResponse::IGNORED, writeResponse->type());
  EXPECT_FALSE(writeResponse->okay());
  EXPECT_EQ(3u, writeResponse->proposal());
  EXPECT_EQ(1u, writeResponse->position());
}


class CoordinatorTest : public TemporaryDirectoryTest
{
protected:
  // Used to change the status of a replicated log from `EMPTY` to `VOTING`.
  tool::Initialize initializer;
};


TEST_F(CoordinatorTest, Elect)
{
  const string path1 = os::getcwd() + "/.log1";
  initializer.flags.path = path1;
  ASSERT_SOME(initializer.execute());

  const string path2 = os::getcwd() + "/.log2";
  initializer.flags.path = path2;
  ASSERT_SOME(initializer.execute());

  Shared<Replica> replica1(new Replica(path1));
  Shared<Replica> replica2(new Replica(path2));

  set<UPID> pids;
  pids.insert(replica1->pid());
  pids.insert(replica2->pid());

  Shared<Network> network(new Network(pids));

  Coordinator coord(2, replica1, network);

  {
    Future<Option<uint64_t>> electing = coord.elect();
    AWAIT_READY(electing);
    EXPECT_SOME_EQ(0u, electing.get());
  }

  {
    Future<list<Action>> actions = replica1->read(0, 0);
    AWAIT_READY(actions);
    ASSERT_EQ(1u, actions->size());
    EXPECT_EQ(0u, actions->front().position());
    ASSERT_TRUE(actions->front().has_type());
    ASSERT_EQ(Action::NOP, actions->front().type());
  }
}


// Verifies that a coordinator can get elected with clock paused (no
// retry involved) for an empty log.
TEST_F(CoordinatorTest, ElectWithClockPaused)
{
  Clock::pause();

  const string path1 = os::getcwd() + "/.log1";
  initializer.flags.path = path1;
  ASSERT_SOME(initializer.execute());

  const string path2 = os::getcwd() + "/.log2";
  initializer.flags.path = path2;
  ASSERT_SOME(initializer.execute());

  Shared<Replica> replica1(new Replica(path1));
  Shared<Replica> replica2(new Replica(path2));

  set<UPID> pids;
  pids.insert(replica1->pid());
  pids.insert(replica2->pid());

  Shared<Network> network(new Network(pids));

  Coordinator coord(2, replica1, network);

  {
    Future<Option<uint64_t>> electing = coord.elect();
    AWAIT_READY(electing);
    EXPECT_SOME_EQ(0u, electing.get());
  }

  Clock::resume();
}


TEST_F(CoordinatorTest, AppendRead)
{
  const string path1 = os::getcwd() + "/.log1";
  initializer.flags.path = path1;
  ASSERT_SOME(initializer.execute());

  const string path2 = os::getcwd() + "/.log2";
  initializer.flags.path = path2;
  ASSERT_SOME(initializer.execute());

  Shared<Replica> replica1(new Replica(path1));
  Shared<Replica> replica2(new Replica(path2));

  set<UPID> pids;
  pids.insert(replica1->pid());
  pids.insert(replica2->pid());

  Shared<Network> network(new Network(pids));

  Coordinator coord(2, replica1, network);

  {
    Future<Option<uint64_t>> electing = coord.elect();
    AWAIT_READY(electing);
    EXPECT_SOME_EQ(0u, electing.get());
  }

  uint64_t position;

  {
    Future<Option<uint64_t>> appending = coord.append("hello world");
    AWAIT_READY(appending);
    ASSERT_SOME(appending.get());
    position = appending->get();
    EXPECT_EQ(1u, position);
  }

  {
    Future<list<Action>> actions = replica1->read(position, position);
    AWAIT_READY(actions);
    ASSERT_EQ(1u, actions->size());
    EXPECT_EQ(position, actions->front().position());
    ASSERT_TRUE(actions->front().has_type());
    ASSERT_EQ(Action::APPEND, actions->front().type());
    EXPECT_EQ("hello world", actions->front().append().bytes());
  }
}


TEST_F(CoordinatorTest, AppendReadError)
{
  const string path1 = os::getcwd() + "/.log1";
  initializer.flags.path = path1;
  ASSERT_SOME(initializer.execute());

  const string path2 = os::getcwd() + "/.log2";
  initializer.flags.path = path2;
  ASSERT_SOME(initializer.execute());

  Shared<Replica> replica1(new Replica(path1));
  Shared<Replica> replica2(new Replica(path2));

  set<UPID> pids;
  pids.insert(replica1->pid());
  pids.insert(replica2->pid());

  Shared<Network> network(new Network(pids));

  Coordinator coord(2, replica1, network);

  {
    Future<Option<uint64_t>> electing = coord.elect();
    AWAIT_READY(electing);
    EXPECT_SOME_EQ(0u, electing.get());
  }

  uint64_t position;

  {
    Future<Option<uint64_t>> appending = coord.append("hello world");
    AWAIT_READY(appending);
    ASSERT_SOME(appending.get());
    position = appending->get();
    EXPECT_EQ(1u, position);
  }

  {
    position += 1;
    Future<list<Action>> actions = replica1->read(position, position);
    AWAIT_FAILED(actions);
    EXPECT_EQ("Bad read range (past end of log)", actions.failure());
  }
}


TEST_F(CoordinatorTest, AppendDiscarded)
{
  const string path1 = os::getcwd() + "/.log1";
  initializer.flags.path = path1;
  ASSERT_SOME(initializer.execute());

  const string path2 = os::getcwd() + "/.log2";
  initializer.flags.path = path2;
  ASSERT_SOME(initializer.execute());

  Shared<Replica> replica1(new Replica(path1));
  Shared<Replica> replica2(new Replica(path2));

  set<UPID> pids;
  pids.insert(replica1->pid());
  pids.insert(replica2->pid());

  Shared<Network> network(new Network(pids));

  Coordinator coord(2, replica1, network);

  {
    Future<Option<uint64_t>> electing = coord.elect();
    AWAIT_READY(electing);
    ASSERT_SOME(electing.get());
    EXPECT_EQ(0u, electing->get());
  }

  process::terminate(replica2->pid());
  process::wait(replica2->pid());
  replica2.reset();

  {
    Future<Option<uint64_t>> appending = coord.append("hello world");
    ASSERT_TRUE(appending.isPending());

    appending.discard();
    AWAIT_DISCARDED(appending);
  }

  {
    Future<Option<uint64_t>> appending = coord.append("hello moto");
    AWAIT_READY(appending);

    EXPECT_NONE(appending.get());
  }
}


TEST_F(CoordinatorTest, ElectNoQuorum)
{
  const string path = os::getcwd() + "/.log";
  initializer.flags.path = path;
  ASSERT_SOME(initializer.execute());

  Shared<Replica> replica(new Replica(path));

  set<UPID> pids;
  pids.insert(replica->pid());

  Shared<Network> network(new Network(pids));

  Coordinator coord(2, replica, network);

  Clock::pause();

  Future<Option<uint64_t>> electing = coord.elect();

  Clock::advance(Seconds(10));
  Clock::settle();

  EXPECT_TRUE(electing.isPending());

  Clock::resume();
}


TEST_F(CoordinatorTest, AppendNoQuorum)
{
  const string path1 = os::getcwd() + "/.log1";
  initializer.flags.path = path1;
  ASSERT_SOME(initializer.execute());

  const string path2 = os::getcwd() + "/.log2";
  initializer.flags.path = path2;
  ASSERT_SOME(initializer.execute());

  Shared<Replica> replica1(new Replica(path1));
  Shared<Replica> replica2(new Replica(path2));

  set<UPID> pids;
  pids.insert(replica1->pid());
  pids.insert(replica2->pid());

  Shared<Network> network(new Network(pids));

  Coordinator coord(2, replica1, network);

  {
    Future<Option<uint64_t>> electing = coord.elect();
    AWAIT_READY(electing);
    EXPECT_SOME_EQ(0u, electing.get());
  }

  process::terminate(replica2->pid());
  process::wait(replica2->pid());
  replica2.reset();

  Clock::pause();

  Future<Option<uint64_t>> appending = coord.append("hello world");

  Clock::advance(Seconds(10));
  Clock::settle();

  EXPECT_TRUE(appending.isPending());

  Clock::resume();
}


TEST_F(CoordinatorTest, Failover)
{
  const string path1 = os::getcwd() + "/.log1";
  initializer.flags.path = path1;
  ASSERT_SOME(initializer.execute());

  const string path2 = os::getcwd() + "/.log2";
  initializer.flags.path = path2;
  ASSERT_SOME(initializer.execute());

  Shared<Replica> replica1(new Replica(path1));
  Shared<Replica> replica2(new Replica(path2));

  set<UPID> pids;
  pids.insert(replica1->pid());
  pids.insert(replica2->pid());

  Shared<Network> network1(new Network(pids));

  Coordinator coord1(2, replica1, network1);

  {
    Future<Option<uint64_t>> electing = coord1.elect();
    AWAIT_READY(electing);
    EXPECT_SOME_EQ(0u, electing.get());
  }

  uint64_t position;

  {
    Future<Option<uint64_t>> appending = coord1.append("hello world");
    AWAIT_READY(appending);
    ASSERT_SOME(appending.get());
    position = appending->get();
    EXPECT_EQ(1u, position);
  }

  Shared<Network> network2(new Network(pids));

  Coordinator coord2(2, replica2, network2);

  {
    Future<Option<uint64_t>> electing = coord2.elect();
    AWAIT_READY(electing);
    EXPECT_SOME_EQ(position, electing.get());
  }

  {
    Future<list<Action>> actions = replica2->read(position, position);
    AWAIT_READY(actions);
    ASSERT_EQ(1u, actions->size());
    EXPECT_EQ(position, actions->front().position());
    ASSERT_TRUE(actions->front().has_type());
    ASSERT_EQ(Action::APPEND, actions->front().type());
    EXPECT_EQ("hello world", actions->front().append().bytes());
  }
}


TEST_F(CoordinatorTest, Demoted)
{
  const string path1 = os::getcwd() + "/.log1";
  initializer.flags.path = path1;
  ASSERT_SOME(initializer.execute());

  const string path2 = os::getcwd() + "/.log2";
  initializer.flags.path = path2;
  ASSERT_SOME(initializer.execute());

  Shared<Replica> replica1(new Replica(path1));
  Shared<Replica> replica2(new Replica(path2));

  set<UPID> pids;
  pids.insert(replica1->pid());
  pids.insert(replica2->pid());

  Shared<Network> network1(new Network(pids));

  Coordinator coord1(2, replica1, network1);

  {
    Future<Option<uint64_t>> electing = coord1.elect();
    AWAIT_READY(electing);
    EXPECT_SOME_EQ(0u, electing.get());
  }

  uint64_t position1;

  {
    Future<Option<uint64_t>> appending = coord1.append("hello world");
    AWAIT_READY(appending);
    ASSERT_SOME(appending.get());
    position1 = appending->get();
    EXPECT_EQ(1u, position1);
  }

  Shared<Network> network2(new Network(pids));

  Coordinator coord2(2, replica2, network2);

  {
    Future<Option<uint64_t>> electing = coord2.elect();
    AWAIT_READY(electing);
    EXPECT_SOME_EQ(position1, electing.get());
  }

  {
    Future<Option<uint64_t>> appending = coord1.append("hello moto");
    AWAIT_READY(appending);
    EXPECT_NONE(appending.get());
  }

  uint64_t position2;

  {
    Future<Option<uint64_t>> appending = coord2.append("hello hello");
    AWAIT_READY(appending);
    ASSERT_SOME(appending.get());
    position2 = appending->get();
    EXPECT_EQ(2u, position2);
  }

  {
    Future<list<Action>> actions = replica2->read(position2, position2);
    AWAIT_READY(actions);
    ASSERT_EQ(1u, actions->size());
    EXPECT_EQ(position2, actions->front().position());
    ASSERT_TRUE(actions->front().has_type());
    ASSERT_EQ(Action::APPEND, actions->front().type());
    EXPECT_EQ("hello hello", actions->front().append().bytes());
  }
}


TEST_F(CoordinatorTest, Fill)
{
  const string path1 = os::getcwd() + "/.log1";
  initializer.flags.path = path1;
  ASSERT_SOME(initializer.execute());

  const string path2 = os::getcwd() + "/.log2";
  initializer.flags.path = path2;
  ASSERT_SOME(initializer.execute());

  const string path3 = os::getcwd() + "/.log3";
  initializer.flags.path = path3;
  ASSERT_SOME(initializer.execute());

  Shared<Replica> replica1(new Replica(path1));
  Shared<Replica> replica2(new Replica(path2));

  set<UPID> pids;
  pids.insert(replica1->pid());
  pids.insert(replica2->pid());

  Shared<Network> network1(new Network(pids));

  Coordinator coord1(2, replica1, network1);

  {
    Future<Option<uint64_t>> electing = coord1.elect();
    AWAIT_READY(electing);
    EXPECT_SOME_EQ(0u, electing.get());
  }

  uint64_t position;

  {
    Future<Option<uint64_t>> appending = coord1.append("hello world");
    AWAIT_READY(appending);
    ASSERT_SOME(appending.get());
    position = appending->get();
    EXPECT_EQ(1u, position);
  }

  Shared<Replica> replica3(new Replica(path3));

  pids.clear();
  pids.insert(replica2->pid());
  pids.insert(replica3->pid());

  Shared<Network> network2(new Network(pids));

  Coordinator coord2(2, replica3, network2);

  {
    // Note that the first election should fail because 'coord2' gets
    // its proposal number from 'replica3' which has an empty log and
    // thus a second attempt will need to be made.
    Future<Option<uint64_t>> electing = coord2.elect();
    AWAIT_READY(electing);
    ASSERT_NONE(electing.get());

    electing = coord2.elect();
    AWAIT_READY(electing);
    EXPECT_SOME_EQ(position, electing.get());
  }

  {
    Future<list<Action>> actions = replica3->read(position, position);
    AWAIT_READY(actions);
    ASSERT_EQ(1u, actions->size());
    EXPECT_EQ(position, actions->front().position());
    ASSERT_TRUE(actions->front().has_type());
    ASSERT_EQ(Action::APPEND, actions->front().type());
    EXPECT_EQ("hello world", actions->front().append().bytes());
  }
}


TEST_F(CoordinatorTest, NotLearnedFill)
{
  const string path1 = os::getcwd() + "/.log1";
  initializer.flags.path = path1;
  ASSERT_SOME(initializer.execute());

  const string path2 = os::getcwd() + "/.log2";
  initializer.flags.path = path2;
  ASSERT_SOME(initializer.execute());

  const string path3 = os::getcwd() + "/.log3";
  initializer.flags.path = path3;
  ASSERT_SOME(initializer.execute());

  Shared<Replica> replica1(new Replica(path1));
  Shared<Replica> replica2(new Replica(path2));

  // Drop messages here in order to obtain the pid of replica2. We
  // only want to drop learned message sent to replica2.
  DROP_PROTOBUFS(LearnedMessage(), _, Eq(replica2->pid()));

  set<UPID> pids;
  pids.insert(replica1->pid());
  pids.insert(replica2->pid());

  Shared<Network> network1(new Network(pids));

  Coordinator coord1(2, replica1, network1);

  {
    Future<Option<uint64_t>> electing = coord1.elect();
    AWAIT_READY(electing);
    EXPECT_SOME_EQ(0u, electing.get());
  }

  uint64_t position;

  {
    Future<Option<uint64_t>> appending = coord1.append("hello world");
    AWAIT_READY(appending);
    ASSERT_SOME(appending.get());
    position = appending->get();
    EXPECT_EQ(1u, position);
  }

  Shared<Replica> replica3(new Replica(path3));

  pids.clear();
  pids.insert(replica2->pid());
  pids.insert(replica3->pid());

  Shared<Network> network2(new Network(pids));

  Coordinator coord2(2, replica3, network2);

  {
    // Note that the first election should fail because 'coord2' gets
    // its proposal number from 'replica3' which has an empty log and
    // thus a second attempt will need to be made.
    Future<Option<uint64_t>> electing = coord2.elect();
    AWAIT_READY(electing);
    ASSERT_NONE(electing.get());

    electing = coord2.elect();
    AWAIT_READY(electing);
    EXPECT_SOME_EQ(position, electing.get());
  }

  {
    Future<list<Action>> actions = replica3->read(position, position);
    AWAIT_READY(actions);
    ASSERT_EQ(1u, actions->size());
    EXPECT_EQ(position, actions->front().position());
    ASSERT_TRUE(actions->front().has_type());
    ASSERT_EQ(Action::APPEND, actions->front().type());
    EXPECT_EQ("hello world", actions->front().append().bytes());
  }
}


TEST_F(CoordinatorTest, MultipleAppends)
{
  const string path1 = os::getcwd() + "/.log1";
  initializer.flags.path = path1;
  ASSERT_SOME(initializer.execute());

  const string path2 = os::getcwd() + "/.log2";
  initializer.flags.path = path2;
  ASSERT_SOME(initializer.execute());

  Shared<Replica> replica1(new Replica(path1));
  Shared<Replica> replica2(new Replica(path2));

  set<UPID> pids;
  pids.insert(replica1->pid());
  pids.insert(replica2->pid());

  Shared<Network> network(new Network(pids));

  Coordinator coord(2, replica1, network);

  {
    Future<Option<uint64_t>> electing = coord.elect();
    AWAIT_READY(electing);
    EXPECT_SOME_EQ(0u, electing.get());
  }

  for (uint64_t position = 1; position <= 10; position++) {
    Future<Option<uint64_t>> appending = coord.append(stringify(position));
    AWAIT_READY(appending);
    EXPECT_SOME_EQ(position, appending.get());
  }

  {
    Future<list<Action>> actions = replica1->read(1, 10);
    AWAIT_READY(actions);
    EXPECT_EQ(10u, actions->size());
    foreach (const Action& action, actions.get()) {
      ASSERT_TRUE(action.has_type());
      ASSERT_EQ(Action::APPEND, action.type());
      EXPECT_EQ(stringify(action.position()), action.append().bytes());
    }
  }
}


TEST_F(CoordinatorTest, MultipleAppendsNotLearnedFill)
{
  const string path1 = os::getcwd() + "/.log1";
  initializer.flags.path = path1;
  ASSERT_SOME(initializer.execute());

  const string path2 = os::getcwd() + "/.log2";
  initializer.flags.path = path2;
  ASSERT_SOME(initializer.execute());

  const string path3 = os::getcwd() + "/.log3";
  initializer.flags.path = path3;
  ASSERT_SOME(initializer.execute());

  Shared<Replica> replica1(new Replica(path1));
  Shared<Replica> replica2(new Replica(path2));

  // Drop messages here in order to obtain the pid of replica2. We
  // only want to drop learned message sent to replica2.
  DROP_PROTOBUFS(LearnedMessage(), _, Eq(replica2->pid()));

  set<UPID> pids;
  pids.insert(replica1->pid());
  pids.insert(replica2->pid());

  Shared<Network> network1(new Network(pids));

  Coordinator coord1(2, replica1, network1);

  {
    Future<Option<uint64_t>> electing = coord1.elect();
    AWAIT_READY(electing);
    EXPECT_SOME_EQ(0u, electing.get());
  }

  for (uint64_t position = 1; position <= 10; position++) {
    Future<Option<uint64_t>> appending = coord1.append(stringify(position));
    AWAIT_READY(appending);
    EXPECT_SOME_EQ(position, appending.get());
  }

  Shared<Replica> replica3(new Replica(path3));

  pids.clear();
  pids.insert(replica2->pid());
  pids.insert(replica3->pid());

  Shared<Network> network2(new Network(pids));

  Coordinator coord2(2, replica3, network2);

  {
    // Note that the first election should fail because 'coord2' gets
    // its proposal number from 'replica3' which has an empty log and
    // thus a second attempt will need to be made.
    Future<Option<uint64_t>> electing = coord2.elect();
    AWAIT_READY(electing);
    ASSERT_NONE(electing.get());

    electing = coord2.elect();
    AWAIT_READY(electing);
    EXPECT_SOME_EQ(10u, electing.get());
  }

  {
    Future<list<Action>> actions = replica3->read(1, 10);
    AWAIT_READY(actions);
    EXPECT_EQ(10u, actions->size());
    foreach (const Action& action, actions.get()) {
      ASSERT_TRUE(action.has_type());
      ASSERT_EQ(Action::APPEND, action.type());
      EXPECT_EQ(stringify(action.position()), action.append().bytes());
    }
  }
}


TEST_F(CoordinatorTest, Truncate)
{
  const string path1 = os::getcwd() + "/.log1";
  initializer.flags.path = path1;
  ASSERT_SOME(initializer.execute());

  const string path2 = os::getcwd() + "/.log2";
  initializer.flags.path = path2;
  ASSERT_SOME(initializer.execute());

  Shared<Replica> replica1(new Replica(path1));
  Shared<Replica> replica2(new Replica(path2));

  set<UPID> pids;
  pids.insert(replica1->pid());
  pids.insert(replica2->pid());

  Shared<Network> network(new Network(pids));

  Coordinator coord(2, replica1, network);

  {
    Future<Option<uint64_t>> electing = coord.elect();
    AWAIT_READY(electing);
    EXPECT_SOME_EQ(0u, electing.get());
  }

  for (uint64_t position = 1; position <= 10; position++) {
    Future<Option<uint64_t>> appending = coord.append(stringify(position));
    AWAIT_READY(appending);
    EXPECT_SOME_EQ(position, appending.get());
  }

  {
    Future<Option<uint64_t>> truncating = coord.truncate(7);
    AWAIT_READY(truncating);
    EXPECT_SOME_EQ(11u, truncating.get());
  }

  {
    Future<list<Action>> actions = replica1->read(6, 10);
    AWAIT_FAILED(actions);
    EXPECT_EQ("Bad read range (truncated position)", actions.failure());
  }

  {
    Future<list<Action>> actions = replica1->read(7, 10);
    AWAIT_READY(actions);
    EXPECT_EQ(4u, actions->size());
    foreach (const Action& action, actions.get()) {
      ASSERT_TRUE(action.has_type());
      ASSERT_EQ(Action::APPEND, action.type());
      EXPECT_EQ(stringify(action.position()), action.append().bytes());
    }
  }
}


TEST_F(CoordinatorTest, TruncateNotLearnedFill)
{
  const string path1 = os::getcwd() + "/.log1";
  initializer.flags.path = path1;
  ASSERT_SOME(initializer.execute());

  const string path2 = os::getcwd() + "/.log2";
  initializer.flags.path = path2;
  ASSERT_SOME(initializer.execute());

  const string path3 = os::getcwd() + "/.log3";
  initializer.flags.path = path3;
  ASSERT_SOME(initializer.execute());

  Shared<Replica> replica1(new Replica(path1));
  Shared<Replica> replica2(new Replica(path2));

  // Drop messages here in order to obtain the pid of replica2. We
  // only want to drop learned message sent to replica2.
  DROP_PROTOBUFS(LearnedMessage(), _, Eq(replica2->pid()));

  set<UPID> pids;
  pids.insert(replica1->pid());
  pids.insert(replica2->pid());

  Shared<Network> network1(new Network(pids));

  Coordinator coord1(2, replica1, network1);

  {
    Future<Option<uint64_t>> electing = coord1.elect();
    AWAIT_READY(electing);
    EXPECT_SOME_EQ(0u, electing.get());
  }

  for (uint64_t position = 1; position <= 10; position++) {
    Future<Option<uint64_t>> appending = coord1.append(stringify(position));
    AWAIT_READY(appending);
    EXPECT_SOME_EQ(position, appending.get());
  }

  {
    Future<Option<uint64_t>> truncating = coord1.truncate(7);
    AWAIT_READY(truncating);
    EXPECT_SOME_EQ(11u, truncating.get());
  }

  Shared<Replica> replica3(new Replica(path3));

  pids.clear();
  pids.insert(replica2->pid());
  pids.insert(replica3->pid());

  Shared<Network> network2(new Network(pids));

  Coordinator coord2(2, replica3, network2);

  {
    // Note that the first election should fail because 'coord2' gets
    // its proposal number from 'replica3' which has an empty log and
    // thus a second attempt will need to be made.
    Future<Option<uint64_t>> electing = coord2.elect();
    AWAIT_READY(electing);
    ASSERT_NONE(electing.get());

    electing = coord2.elect();
    AWAIT_READY(electing);
    EXPECT_SOME_EQ(11u, electing.get());
  }

  {
    Future<list<Action>> actions = replica3->read(6, 10);
    AWAIT_FAILED(actions);
    EXPECT_EQ("Bad read range (truncated position)", actions.failure());
  }

  {
    Future<list<Action>> actions = replica3->read(7, 10);
    AWAIT_READY(actions);
    EXPECT_EQ(4u, actions->size());
    foreach (const Action& action, actions.get()) {
      ASSERT_TRUE(action.has_type());
      ASSERT_EQ(Action::APPEND, action.type());
      EXPECT_EQ(stringify(action.position()), action.append().bytes());
    }
  }
}


TEST_F(CoordinatorTest, TruncateLearnedFill)
{
  const string path1 = os::getcwd() + "/.log1";
  initializer.flags.path = path1;
  ASSERT_SOME(initializer.execute());

  const string path2 = os::getcwd() + "/.log2";
  initializer.flags.path = path2;
  ASSERT_SOME(initializer.execute());

  const string path3 = os::getcwd() + "/.log3";
  initializer.flags.path = path3;
  ASSERT_SOME(initializer.execute());

  Shared<Replica> replica1(new Replica(path1));
  Shared<Replica> replica2(new Replica(path2));

  set<UPID> pids;
  pids.insert(replica1->pid());
  pids.insert(replica2->pid());

  Shared<Network> network1(new Network(pids));

  Coordinator coord1(2, replica1, network1);

  {
    Future<Option<uint64_t>> electing = coord1.elect();
    AWAIT_READY(electing);
    EXPECT_SOME_EQ(0u, electing.get());
  }

  for (uint64_t position = 1; position <= 10; position++) {
    Future<Option<uint64_t>> appending = coord1.append(stringify(position));
    AWAIT_READY(appending);
    EXPECT_SOME_EQ(position, appending.get());
  }

  {
    Future<Option<uint64_t>> truncating = coord1.truncate(7);
    AWAIT_READY(truncating);
    EXPECT_SOME_EQ(11u, truncating.get());
  }

  Shared<Replica> replica3(new Replica(path3));

  pids.clear();
  pids.insert(replica2->pid());
  pids.insert(replica3->pid());

  Shared<Network> network2(new Network(pids));

  Coordinator coord2(2, replica3, network2);

  {
    // Note that the first election should fail because 'coord2' gets
    // its proposal number from 'replica3' which has an empty log and
    // thus a second attempt will need to be made.
    Future<Option<uint64_t>> electing = coord2.elect();
    AWAIT_READY(electing);
    ASSERT_NONE(electing.get());

    electing = coord2.elect();
    AWAIT_READY(electing);
    EXPECT_SOME_EQ(11u, electing.get());
  }

  {
    Future<list<Action>> actions = replica3->read(6, 10);
    AWAIT_FAILED(actions);
    EXPECT_EQ("Bad read range (truncated position)", actions.failure());
  }

  {
    Future<list<Action>> actions = replica3->read(7, 10);
    AWAIT_READY(actions);
    EXPECT_EQ(4u, actions->size());
    foreach (const Action& action, actions.get()) {
      ASSERT_TRUE(action.has_type());
      ASSERT_EQ(Action::APPEND, action.type());
      EXPECT_EQ(stringify(action.position()), action.append().bytes());
    }
  }
}


class MockReplica : public Replica
{
public:
  explicit MockReplica(const string& path) :
    Replica(path) {}

  ~MockReplica() override {}

  MOCK_METHOD1(update, Future<bool>(const Metadata::Status& status));

  Future<bool> _update(const Metadata::Status& status)
  {
    return Replica::update(status);
  }
};


// If a coordinator tries to get elected while there is not a quorum
// of replicas in VOTING state, the non-VOTING replicas should
// instruct the coordinator that they have ignored the coordinator's
// request, so the coordinator can promptly retry. MESOS-3280.
TEST_F(CoordinatorTest, RecoveryRace)
{
  const string path1 = os::getcwd() + "/.log1";
  const string path2 = os::getcwd() + "/.log2";
  const string path3 = os::getcwd() + "/.log3";

  MockReplica* replica1(new MockReplica(path1));
  MockReplica* replica2(new MockReplica(path2));
  MockReplica* replica3(new MockReplica(path3));

  set<UPID> pids{replica1->pid(), replica2->pid(), replica3->pid()};
  Shared<Network> network(new Network(pids));

  // Set when each replica transitions from EMPTY -> STARTING; the
  // replica will then block until the associated "continue" promise
  // is set.
  Future<Nothing> replica1Starting;
  Future<Nothing> replica2Starting;
  Future<Nothing> replica3Starting;
  process::Promise<bool> replica1ContinueStarting;
  process::Promise<bool> replica2ContinueStarting;
  process::Promise<bool> replica3ContinueStarting;

  // Set when each replica transitions from STARTING -> VOTING.
  Future<Nothing> replica1Voting;
  Future<Nothing> replica2Voting;
  process::Promise<bool> replica1ContinueVoting;
  process::Promise<bool> replica2ContinueVoting;

  // Arrange mocks to allow us to block and unblock each replica when
  // it changes state.
  // TODO(neilc): Refactor this to reduce duplicated code.
  EXPECT_CALL(*replica1, update(_))
    .WillOnce(DoAll(IgnoreResult(Invoke(replica1, &MockReplica::_update)),
                    FutureSatisfy(&replica1Starting),
                    Return(replica1ContinueStarting.future())))
    .WillOnce(DoAll(IgnoreResult(Invoke(replica1, &MockReplica::_update)),
                    FutureSatisfy(&replica1Voting),
                    Return(replica1ContinueVoting.future())));
  EXPECT_CALL(*replica2, update(_))
    .WillOnce(DoAll(IgnoreResult(Invoke(replica2, &MockReplica::_update)),
                    FutureSatisfy(&replica2Starting),
                    Return(replica2ContinueStarting.future())))
    .WillOnce(DoAll(IgnoreResult(Invoke(replica2, &MockReplica::_update)),
                    FutureSatisfy(&replica2Voting),
                    Return(replica2ContinueVoting.future())));
  EXPECT_CALL(*replica3, update(_))
    .WillOnce(DoAll(IgnoreResult(Invoke(replica3, &MockReplica::_update)),
                    FutureSatisfy(&replica3Starting),
                    Return(replica3ContinueStarting.future())))
    .WillRepeatedly(Invoke(replica3, &MockReplica::_update));

  Future<Owned<Replica>> recovering1 =
    recover(2, Owned<Replica>(replica1), network, true);
  Future<Owned<Replica>> recovering2 =
    recover(2, Owned<Replica>(replica2), network, true);
  Future<Owned<Replica>> recovering3 =
    recover(2, Owned<Replica>(replica3), network, true);

  AWAIT_READY(replica1Starting);
  AWAIT_READY(replica2Starting);
  AWAIT_READY(replica3Starting);

  // Allow replica1 to advance from STARTING -> VOTING.
  // TODO(neilc): Due to an apparent bug in FutureResult (MESOS-3812),
  // we can't save the return value of _update() when setting up the
  // mocks above. Hence, we have to assume that _update() returned
  // true, which we then use to unblock the process.
  replica1ContinueStarting.set(true);
  AWAIT_READY(replica1Voting);
  replica1ContinueVoting.set(true);

  AWAIT_READY(recovering1);
  Owned<Replica> shared_ = recovering1.get();
  Shared<Replica> shared = shared_.share();

  // Electing a coordinator should fail because we don't have a quorum
  // of replicas in VOTING status.
  {
    Coordinator coord1(2, shared, network);
    Future<Option<uint64_t>> electing = coord1.elect();
    AWAIT_READY(electing);
    ASSERT_NONE(electing.get());
  }

  // Allow replica2 to advance from STARTING -> VOTING.
  replica2ContinueStarting.set(true);
  AWAIT_READY(replica2Voting);
  replica2ContinueVoting.set(true);

  AWAIT_READY(recovering2);

  // Electing a coordinator should now succeed.
  Coordinator coord2(2, shared, network);
  {
    Future<Option<uint64_t>> electing = coord2.elect();
    AWAIT_READY(electing);
    EXPECT_SOME_EQ(0u, electing.get());
  }

  // Allow replica3 to advance from STARTING -> RECOVERING -> VOTING.
  // TODO(neilc): Transition to RECOVERING is dubious and should
  // probably be omitted.
  replica3ContinueStarting.set(true);

  AWAIT_READY(recovering3);

  {
    Future<Option<uint64_t>> appending = coord2.append("hello world");
    AWAIT_READY(appending);
    EXPECT_SOME_EQ(1u, appending.get());
  }
}


class RecoverTest : public TemporaryDirectoryTest
{
protected:
  // Used to change the status of a replicated log from `EMPTY` to `VOTING`.
  tool::Initialize initializer;
};


// Two logs both need recovery compete with each other.
TEST_F(RecoverTest, RacingCatchup)
{
  const string path1 = os::getcwd() + "/.log1";
  initializer.flags.path = path1;
  ASSERT_SOME(initializer.execute());

  const string path2 = os::getcwd() + "/.log2";
  initializer.flags.path = path2;
  ASSERT_SOME(initializer.execute());

  const string path3 = os::getcwd() + "/.log3";
  initializer.flags.path = path3;
  ASSERT_SOME(initializer.execute());

  const string path4 = os::getcwd() + "/.log4";
  const string path5 = os::getcwd() + "/.log5";

  Shared<Replica> replica1(new Replica(path1));
  Shared<Replica> replica2(new Replica(path2));
  Shared<Replica> replica3(new Replica(path3));

  set<UPID> pids;
  pids.insert(replica1->pid());
  pids.insert(replica2->pid());
  pids.insert(replica3->pid());

  Shared<Network> network1(new Network(pids));

  Coordinator coord1(3, replica1, network1);

  {
    Future<Option<uint64_t>> electing = coord1.elect();
    AWAIT_READY(electing);
    EXPECT_SOME_EQ(0u, electing.get());
  }

  for (uint64_t position = 1; position <= 10; position++) {
    Future<Option<uint64_t>> appending = coord1.append(stringify(position));
    AWAIT_READY(appending);
    EXPECT_SOME_EQ(position, appending.get());
  }

  // Two replicas both want to recover.
  Owned<Replica> replica4(new Replica(path4));
  Owned<Replica> replica5(new Replica(path5));

  pids.insert(replica4->pid());
  pids.insert(replica5->pid());

  Shared<Network> network2(new Network(pids));

  Future<Owned<Replica>> recovering4 = recover(3, replica4, network2);
  Future<Owned<Replica>> recovering5 = recover(3, replica5, network2);

  // Wait until recovery is done.
  AWAIT_READY(recovering4);
  AWAIT_READY(recovering5);

  Owned<Replica> shared4_ = recovering4.get();
  Shared<Replica> shared4 = shared4_.share();
  Coordinator coord2(3, shared4, network2);

  {
    // Note that the first election should fail because 'coord2' gets
    // its proposal number from 'replica3' which has an empty log and
    // thus a second attempt will need to be made.
    Future<Option<uint64_t>> electing = coord2.elect();
    AWAIT_READY(electing);
    ASSERT_NONE(electing.get());

    electing = coord2.elect();
    AWAIT_READY(electing);
    EXPECT_SOME_EQ(10u, electing.get());
  }

  {
    Future<list<Action>> actions = shared4->read(1, 10);
    AWAIT_READY(actions);
    EXPECT_EQ(10u, actions->size());
    foreach (const Action& action, actions.get()) {
      ASSERT_TRUE(action.has_type());
      ASSERT_EQ(Action::APPEND, action.type());
      EXPECT_EQ(stringify(action.position()), action.append().bytes());
    }
  }

  {
    Future<Option<uint64_t>> appending = coord2.append("hello hello");
    AWAIT_READY(appending);
    EXPECT_SOME_EQ(11u, appending.get());
  }

  {
    Future<list<Action>> actions = shared4->read(11u, 11u);
    AWAIT_READY(actions);
    ASSERT_EQ(1u, actions->size());
    EXPECT_EQ(11u, actions->front().position());
    ASSERT_TRUE(actions->front().has_type());
    ASSERT_EQ(Action::APPEND, actions->front().type());
    EXPECT_EQ("hello hello", actions->front().append().bytes());
  }
}


TEST_F(RecoverTest, CatchupRetry)
{
  const string path1 = os::getcwd() + "/.log1";
  initializer.flags.path = path1;
  ASSERT_SOME(initializer.execute());

  const string path2 = os::getcwd() + "/.log2";
  initializer.flags.path = path2;
  ASSERT_SOME(initializer.execute());

  const string path3 = os::getcwd() + "/.log3";

  Shared<Replica> replica1(new Replica(path1));
  Shared<Replica> replica2(new Replica(path2));

  // Make sure replica2 does not receive learned messages.
  DROP_PROTOBUFS(LearnedMessage(), _, Eq(replica2->pid()));

  set<UPID> pids;
  pids.insert(replica1->pid());
  pids.insert(replica2->pid());

  Shared<Network> network1(new Network(pids));

  Coordinator coord(2, replica1, network1);

  {
    Future<Option<uint64_t>> electing = coord.elect();
    AWAIT_READY(electing);
    EXPECT_SOME_EQ(0u, electing.get());
  }

  IntervalSet<uint64_t> positions;

  for (uint64_t position = 1; position <= 10; position++) {
    Future<Option<uint64_t>> appending = coord.append(stringify(position));
    AWAIT_READY(appending);
    EXPECT_SOME_EQ(position, appending.get());
    positions += position;
  }

  Shared<Replica> replica3(new Replica(path3));

  pids.insert(replica3->pid());

  Shared<Network> network2(new Network(pids));

  // Drop a promise request to replica1 so that the catch-up process
  // won't be able to get a quorum of explicit promises. Also, since
  // learned messages are blocked from being sent replica2, the
  // catch-up process has to wait for a quorum of explicit promises.
  // If we don't allow retry, the catch-up process will get stuck at
  // promise phase even if replica1 reemerges later.
  DROP_PROTOBUF(PromiseRequest(), _, Eq(replica1->pid()));

  Future<Nothing> catching =
    catchup(2, replica3, network2, None(), positions, Seconds(10));

  Clock::pause();

  // Wait for the retry timer in 'catchup' to be setup.
  Clock::settle();

  // Wait for the proposal number to be bumped.
  Clock::advance(Seconds(1));
  Clock::settle();

  // Wait for 'catchup' to retry.
  Clock::advance(Seconds(10));
  Clock::settle();

  // Wait for another proposal number bump.
  Clock::advance(Seconds(1));
  Clock::settle();

  Clock::resume();

  AWAIT_READY(catching);
}


TEST_F(RecoverTest, RecoverProtocolRetry)
{
  const string path1 = path::join(os::getcwd(), ".log1");
  initializer.flags.path = path1;
  ASSERT_SOME(initializer.execute());

  const string path2 = path::join(os::getcwd(), ".log2");
  const string path3 = path::join(os::getcwd(), ".log3");

  Owned<Replica> replica1(new Replica(path1));
  Owned<Replica> replica2(new Replica(path2));
  Owned<Replica> replica3(new Replica(path3));

  set<UPID> pids{replica1->pid(), replica2->pid(), replica3->pid()};
  Shared<Network> network(new Network(pids));

  Future<Owned<Replica>> recovering = recover(2, replica3, network);

  Clock::pause();

  // Wait for the retry timer to be setup.
  Clock::settle();
  ASSERT_TRUE(recovering.isPending());

  // Wait for recover process to retry.
  Clock::advance(Seconds(10));
  Clock::settle();
  ASSERT_TRUE(recovering.isPending());

  // Remove replica 2 from the network to be initialized. It is safe
  // to have non-const access to shared Network here, because all
  // Network operations are serialized through a Process.
  const_cast<Network&>(*network).remove(replica2->pid());
  replica2.reset();

  initializer.flags.path = path2;
  ASSERT_SOME(initializer.execute());

  replica2.reset(new Replica(path2));
  const_cast<Network&>(*network).add(replica2->pid());

  // Wait for recover process to retry again, now with 2 VOTING
  // replicas. It should successfully finish now.
  Clock::advance(Seconds(10));
  Clock::resume();

  AWAIT_READY(recovering);
}


TEST_F(RecoverTest, AutoInitialization)
{
  const string path1 = os::getcwd() + "/.log1";
  const string path2 = os::getcwd() + "/.log2";
  const string path3 = os::getcwd() + "/.log3";

  Owned<Replica> replica1(new Replica(path1));
  Owned<Replica> replica2(new Replica(path2));
  Owned<Replica> replica3(new Replica(path3));

  set<UPID> pids;
  pids.insert(replica1->pid());
  pids.insert(replica2->pid());
  pids.insert(replica3->pid());

  Shared<Network> network(new Network(pids));

  Future<Owned<Replica>> recovering1 = recover(2, replica1, network, true);
  Future<Owned<Replica>> recovering2 = recover(2, replica2, network, true);

  // Verifies that replica1 and replica2 cannot transit into VOTING
  // status because replica3 is still in EMPTY status. We flush the
  // event queue before checking.
  Clock::pause();
  Clock::settle();
  Clock::resume();

  EXPECT_TRUE(recovering1.isPending());
  EXPECT_TRUE(recovering2.isPending());

  Future<Owned<Replica>> recovering3 = recover(2, replica3, network, true);

  Clock::pause();
  Clock::settle();

  // At this moment `replica1` and `replica2` are in EMPTY status, and
  // are retrying with a random interval between [0.5 sec, 1 sec]. Since
  // the retry interval is hard coded and is not configurable, we need
  // to advance the clock here to avoid waiting for the backoff time.
  Clock::advance(Seconds(1));
  Clock::resume();

  AWAIT_READY(recovering1);
  AWAIT_READY(recovering2);
  AWAIT_READY(recovering3);

  Owned<Replica> shared_ = recovering1.get();
  Shared<Replica> shared = shared_.share();

  Coordinator coord(2, shared, network);

  {
    Future<Option<uint64_t>> electing = coord.elect();
    AWAIT_READY(electing);
    EXPECT_SOME_EQ(0u, electing.get());
  }

  {
    Future<Option<uint64_t>> appending = coord.append("hello world");
    AWAIT_READY(appending);
    EXPECT_SOME_EQ(1u, appending.get());
  }

  {
    Future<list<Action>> actions = shared->read(1, 1);
    AWAIT_READY(actions);
    ASSERT_EQ(1u, actions->size());
    EXPECT_EQ(1u, actions->front().position());
    ASSERT_TRUE(actions->front().has_type());
    ASSERT_EQ(Action::APPEND, actions->front().type());
    EXPECT_EQ("hello world", actions->front().append().bytes());
  }
}


TEST_F(RecoverTest, AutoInitializationRetry)
{
  const string path1 = os::getcwd() + "/.log1";
  const string path2 = os::getcwd() + "/.log2";
  const string path3 = os::getcwd() + "/.log3";

  Owned<Replica> replica1(new Replica(path1));
  Owned<Replica> replica2(new Replica(path2));
  Owned<Replica> replica3(new Replica(path3));

  set<UPID> pids;
  pids.insert(replica1->pid());
  pids.insert(replica2->pid());
  pids.insert(replica3->pid());

  Shared<Network> network(new Network(pids));

  // Simulate the case where replica3 is temporarily removed.
  DROP_PROTOBUF(RecoverRequest(), _, Eq(replica3->pid()));
  DROP_PROTOBUF(RecoverRequest(), _, Eq(replica3->pid()));

  Clock::pause();

  Future<Owned<Replica>> recovering1 = recover(2, replica1, network, true);
  Future<Owned<Replica>> recovering2 = recover(2, replica2, network, true);

  // Flush the event queue.
  Clock::settle();

  EXPECT_TRUE(recovering1.isPending());
  EXPECT_TRUE(recovering2.isPending());

  Future<Owned<Replica>> recovering3 = recover(2, replica3, network, true);

  // Replica1 and replica2 will retry recovery after 10 seconds.
  Clock::advance(Seconds(10));
  Clock::settle();

  Clock::resume();

  AWAIT_READY(recovering1);
  AWAIT_READY(recovering2);
  AWAIT_READY(recovering3);

  Owned<Replica> shared_ = recovering1.get();
  Shared<Replica> shared = shared_.share();

  Coordinator coord(2, shared, network);

  {
    Future<Option<uint64_t>> electing = coord.elect();
    AWAIT_READY(electing);
    EXPECT_SOME_EQ(0u, electing.get());
  }

  {
    Future<Option<uint64_t>> appending = coord.append("hello world");
    AWAIT_READY(appending);
    EXPECT_SOME_EQ(1u, appending.get());
  }

  {
    Future<list<Action>> actions = shared->read(1, 1);
    AWAIT_READY(actions);
    ASSERT_EQ(1u, actions->size());
    EXPECT_EQ(1u, actions->front().position());
    ASSERT_TRUE(actions->front().has_type());
    ASSERT_EQ(Action::APPEND, actions->front().type());
    EXPECT_EQ("hello world", actions->front().append().bytes());
  }
}


TEST_F(RecoverTest, CatchupTruncated)
{
  const string path1 = path::join(os::getcwd(), ".log1");
  initializer.flags.path = path1;
  ASSERT_SOME(initializer.execute());

  const string path2 = path::join(os::getcwd(), ".log2");
  initializer.flags.path = path2;
  ASSERT_SOME(initializer.execute());

  const string path3 = path::join(os::getcwd(), ".log3");

  Shared<Replica> replica1(new Replica(path1));
  Shared<Replica> replica2(new Replica(path2));

  set<UPID> pids{replica1->pid(), replica2->pid()};
  Shared<Network> network1(new Network(pids));

  Coordinator coord(2, replica1, network1);
  Future<Option<uint64_t>> electing = coord.elect();
  AWAIT_READY(electing);
  EXPECT_SOME_EQ(0u, electing.get());

  // Add some positions to the log.
  IntervalSet<uint64_t> positions;
  for (uint64_t position = 1; position <= 10; position++) {
    Future<Option<uint64_t>> appending = coord.append(stringify(position));
    AWAIT_READY(appending);
    EXPECT_SOME_EQ(position, appending.get());
    positions += position;
  }

  // Truncate the log.
  Future<Option<uint64_t>> truncating = coord.truncate(5);
  AWAIT_READY(truncating);
  EXPECT_SOME_EQ(11u, truncating.get());

  Shared<Replica> replica3(new Replica(path3));

  pids.insert(replica3->pid());
  Shared<Network> network2(new Network(pids));

  // Pretend we recovered stale 'begin' position of the log before
  // truncation has happened.
  Future<Nothing> catching = catchup(
      2, replica3, network2, None(), positions, Seconds(10));
  AWAIT_READY(catching);

  AWAIT_EXPECT_EQ(5u, replica3->beginning());
  AWAIT_EXPECT_EQ(10u, replica3->ending());

  // Recreate the replica to verify that storage recovery succeeds.
  // Make sure no one retains a shared pointer to replica3 and thus
  // can prevent the DB from closing before we proceed.
  AWAIT_READY(replica3.own());
  replica3.reset(new Replica(path3));

  AWAIT_EXPECT_EQ(5u, replica3->beginning());
  AWAIT_EXPECT_EQ(10u, replica3->ending());
}


// Verifiy that we can catch-up a following VOTING replica.
TEST_F(RecoverTest, CatchupVoting)
{
  const string path1 = path::join(os::getcwd(), ".log1");
  initializer.flags.path = path1;
  ASSERT_SOME(initializer.execute());

  const string path2 = path::join(os::getcwd(), ".log2");
  initializer.flags.path = path2;
  ASSERT_SOME(initializer.execute());

  const string path3 = path::join(os::getcwd(), ".log3");
  initializer.flags.path = path3;
  ASSERT_SOME(initializer.execute());

  Shared<Replica> replica1(new Replica(path1));
  Shared<Replica> replica2(new Replica(path2));

  set<UPID> pids{replica1->pid(), replica2->pid()};
  Shared<Network> network1(new Network(pids));

  Coordinator coord(2, replica1, network1);
  Future<Option<uint64_t>> electing = coord.elect();
  AWAIT_READY(electing);
  EXPECT_SOME_EQ(0u, electing.get());

  // Add some entries to the log.
  for (uint64_t position = 1; position <= 10; position++) {
    Future<Option<uint64_t>> appending = coord.append(stringify(position));
    AWAIT_READY(appending);
    EXPECT_SOME_EQ(position, appending.get());
  }

  // Truncate the log.
  Future<Option<uint64_t>> truncating = coord.truncate(5);
  AWAIT_READY(truncating);
  EXPECT_SOME_EQ(11u, truncating.get());

  // Create one more replica. It is in VOTING status, but it missed
  // positions adding and truncation.
  Shared<Replica> replica3(new Replica(path3));

  pids.insert(replica3->pid());
  Shared<Network> network2(new Network(pids));

  // Catch-up the VOTING replica for reading. We're using 3 as the
  // quorum size here to simulate recovering a stale lowest position
  // (from the local replica).
  Future<uint64_t> catching = catchup(3, replica3, network2);
  AWAIT_EXPECT_EQ(10u, catching);

  Future<uint64_t> begin = replica3->beginning();
  AWAIT_EXPECT_EQ(5u, begin);

  Future<uint64_t> end = replica3->ending();
  AWAIT_EXPECT_EQ(catching.get(), end);

  Future<list<Action>> actions = replica3->read(begin.get(), end.get());
  AWAIT_READY(actions);
  EXPECT_EQ(end.get() - begin.get() + 1, actions->size());
  foreach (const Action& action, actions.get()) {
    ASSERT_TRUE(action.has_type());
    ASSERT_EQ(Action::APPEND, action.type());
    EXPECT_EQ(stringify(action.position()), action.append().bytes());
  }
}


// Verifiy that we can catch-up a following VOTING replica.
TEST_F(RecoverTest, CatchupVotingWithGap)
{
  const string path1 = path::join(os::getcwd(), ".log1");
  initializer.flags.path = path1;
  ASSERT_SOME(initializer.execute());

  const string path2 = path::join(os::getcwd(), ".log2");
  initializer.flags.path = path2;
  ASSERT_SOME(initializer.execute());

  const string path3 = path::join(os::getcwd(), ".log3");
  initializer.flags.path = path3;
  ASSERT_SOME(initializer.execute());

  Shared<Replica> replica1(new Replica(path1));
  Shared<Replica> replica2(new Replica(path2));

  set<UPID> pids{replica1->pid(), replica2->pid()};
  Shared<Network> network1(new Network(pids));

  Coordinator coord(2, replica1, network1);
  Future<Option<uint64_t>> electing = coord.elect();
  AWAIT_READY(electing);
  EXPECT_SOME_EQ(0u, electing.get());

  // Add some entries to the log.
  for (uint64_t position = 1; position <= 10; position++) {
    Future<Option<uint64_t>> appending = coord.append(stringify(position));
    AWAIT_READY(appending);
    EXPECT_SOME_EQ(position, appending.get());
  }

  // Truncate the log.
  Future<Option<uint64_t>> truncating = coord.truncate(5);
  AWAIT_READY(truncating);
  EXPECT_SOME_EQ(11u, truncating.get());

  // Create one more replica. It is in VOTING status, but it missed
  // positions adding and truncation.
  Shared<Replica> replica3(new Replica(path3));

  pids.insert(replica3->pid());
  Shared<Network> network2(new Network(pids));

  // Make sure replica3 doesn't receive recover request, so we won't
  // recover stale 'begin' position.
  DROP_PROTOBUFS(RecoverRequest(), _, Eq(replica3->pid()));

  // Catch-up the VOTING replica for reading.
  Future<uint64_t> catching = catchup(2, replica3, network2);
  AWAIT_EXPECT_EQ(10u, catching);

  Future<uint64_t> begin = replica3->beginning();
  AWAIT_EXPECT_EQ(5u, begin);

  Future<uint64_t> end = replica3->ending();
  AWAIT_EXPECT_EQ(catching.get(), end);

  Future<list<Action>> actions = replica3->read(begin.get(), end.get());
  AWAIT_READY(actions);
  EXPECT_EQ(end.get() - begin.get() + 1, actions->size());
  foreach (const Action& action, actions.get()) {
    ASSERT_TRUE(action.has_type());
    ASSERT_EQ(Action::APPEND, action.type());
    EXPECT_EQ(stringify(action.position()), action.append().bytes());
  }
}


// Verifiy that catch-up fails if we recover only 1 position.
TEST_F(RecoverTest, CatchupVotingOnePosition)
{
  const string path1 = path::join(os::getcwd(), ".log1");
  initializer.flags.path = path1;
  ASSERT_SOME(initializer.execute());

  const string path2 = path::join(os::getcwd(), ".log2");
  initializer.flags.path = path2;
  ASSERT_SOME(initializer.execute());

  const string path3 = path::join(os::getcwd(), ".log3");
  initializer.flags.path = path3;
  ASSERT_SOME(initializer.execute());

  Shared<Replica> replica1(new Replica(path1));
  Shared<Replica> replica2(new Replica(path2));
  Shared<Replica> replica3(new Replica(path3));

  set<UPID> pids{replica1->pid(), replica2->pid(), replica3->pid()};
  Shared<Network> network(new Network(pids));

  AWAIT_FAILED(catchup(2, replica3, network));
  AWAIT_EXPECT_EQ(0u, replica3->beginning());
  AWAIT_EXPECT_EQ(0u, replica3->ending());
}


class LogTest : public TemporaryDirectoryTest
{
protected:
  // Used to change the status of a replicated log from `EMPTY` to `VOTING`.
  tool::Initialize initializer;
};


TEST_F(LogTest, WriteRead)
{
  const string path1 = os::getcwd() + "/.log1";
  initializer.flags.path = path1;
  ASSERT_SOME(initializer.execute());

  const string path2 = os::getcwd() + "/.log2";
  initializer.flags.path = path2;
  ASSERT_SOME(initializer.execute());

  Replica replica1(path1);

  set<UPID> pids;
  pids.insert(replica1.pid());

  Log log(2, path2, pids);

  Log::Writer writer(&log);

  Future<Option<Log::Position>> start = writer.start();

  AWAIT_READY(start);
  ASSERT_SOME(start.get());

  Future<Option<Log::Position>> position = writer.append("hello world");

  AWAIT_READY(position);
  ASSERT_SOME(position.get());

  Log::Reader reader(&log);

  Future<list<Log::Entry>> entries =
    reader.read(position->get(), position->get());

  AWAIT_READY(entries);

  ASSERT_EQ(1u, entries->size());
  EXPECT_EQ(position->get(), entries->front().position);
  EXPECT_EQ("hello world", entries->front().data);
}


TEST_F(LogTest, Position)
{
  const string path1 = os::getcwd() + "/.log1";
  initializer.flags.path = path1;
  ASSERT_SOME(initializer.execute());

  const string path2 = os::getcwd() + "/.log2";
  initializer.flags.path = path2;
  ASSERT_SOME(initializer.execute());

  Replica replica1(path1);

  set<UPID> pids;
  pids.insert(replica1.pid());

  Log log(2, path2, pids);

  Log::Writer writer(&log);

  Future<Option<Log::Position>> start = writer.start();

  AWAIT_READY(start);
  ASSERT_SOME(start.get());

  Future<Option<Log::Position>> position = writer.append("hello world");

  AWAIT_READY(position);
  ASSERT_SOME(position.get());

  ASSERT_EQ(
      position->get(),
      log.position(position->get().identity()));
}


TEST_F(LogTest, Metrics)
{
  // TODO(jieyu): Added a check for the case where the log is not
  // recovered once MESOS-5626 is resolved. One way to do that is to
  // create a log without running the initializaiton tool.

  const string path = os::getcwd() + "/.log";
  initializer.flags.path = path;
  ASSERT_SOME(initializer.execute());

  Log log(1, path, set<process::UPID>(), false, "prefix/");

  // Make sure the log is recovered. If the log is not recovered, the
  // writer cannot be elected.
  Log::Writer writer(&log);

  Future<Option<Log::Position>> start = writer.start();

  AWAIT_READY(start);
  ASSERT_SOME(start.get());

  JSON::Object snapshot = Metrics();

  ASSERT_EQ(1u, snapshot.values.count("prefix/log/recovered"));
  EXPECT_EQ(1, snapshot.values["prefix/log/recovered"]);

  ASSERT_EQ(1u, snapshot.values.count("prefix/log/ensemble_size"));
  EXPECT_EQ(1, snapshot.values["prefix/log/ensemble_size"]);
}


TEST_F(LogTest, ReaderCatchup)
{
  const string path1 = os::getcwd() + "/.log1";
  initializer.flags.path = path1;
  ASSERT_SOME(initializer.execute());

  const string path2 = os::getcwd() + "/.log2";
  initializer.flags.path = path2;
  ASSERT_SOME(initializer.execute());

  const string path3 = os::getcwd() + "/.log3";
  initializer.flags.path = path3;
  ASSERT_SOME(initializer.execute());

  Shared<Replica> replica1(new Replica(path1));
  Shared<Replica> replica2(new Replica(path2));

  set<UPID> pids{replica1->pid(), replica2->pid()};
  Shared<Network> network(new Network(pids));

  Coordinator coord(2, replica2, network);

  Future<Option<uint64_t>> electing = coord.elect();
  AWAIT_READY(electing);
  EXPECT_SOME_EQ(0u, electing.get());

  // Add some entries to the log.
  for (uint64_t position = 1; position <= 10; position++) {
    Future<Option<uint64_t>> appending = coord.append(stringify(position));
    AWAIT_READY(appending);
    EXPECT_SOME_EQ(position, appending.get());
  }

  Log log3(2, path3, pids);
  Log::Reader reader(&log3);

  // Catch-up the replica that missed positions adding.
  Future<Log::Position> end = reader.catchup();
  AWAIT_READY(end);

  Future<Log::Position> begin = reader.beginning();
  AWAIT_READY(begin);

  // We expect to read 9 entries instead of 10, because the catch-up
  // procedure doesn't catch-up the last recovered position. See
  // comments for RecoverMissingProcess.
  Future<list<Log::Entry>> entries = reader.read(begin.get(), end.get());
  AWAIT_READY(entries);
  ASSERT_EQ(9u, entries->size());

  uint64_t position = 1;
  foreach (const Log::Entry& entry, entries.get()) {
    EXPECT_EQ(stringify(position), entry.data);
    ++position;
  }
}


#ifdef MESOS_HAS_JAVA
// TODO(jieyu): We copy the code from TemporaryDirectoryTest here
// because we cannot inherit from two test fixtures. In this future,
// we need a way to compose multiple test fixtures together.
class LogZooKeeperTest : public ZooKeeperTest
{
protected:
  void SetUp() override
  {
    ZooKeeperTest::SetUp();

    // Save the current working directory.
    cwd = os::getcwd();

    // Create a temporary directory for the test.
    Try<string> directory = environment->mkdtemp();

    ASSERT_SOME(directory) << "Failed to mkdtemp";

    sandbox = directory.get();

    LOG(INFO) << "Using temporary directory '" << sandbox.get() << "'";

    // Run the test out of the temporary directory we created.
    ASSERT_SOME(os::chdir(sandbox.get()))
      << "Failed to chdir into '" << sandbox.get() << "'";
  }

  void TearDown() override
  {
    // Return to previous working directory and cleanup the sandbox.
    ASSERT_SOME(os::chdir(cwd));

    if (sandbox.isSome()) {
      ASSERT_SOME(os::rmdir(sandbox.get()));
    }
  }

  // Used to change the status of a replicated log from `EMPTY` to `VOTING`.
  tool::Initialize initializer;

private:
  string cwd;
  Option<string> sandbox;
};


TEST_F(LogZooKeeperTest, WriteRead)
{
  const string path1 = os::getcwd() + "/.log1";
  initializer.flags.path = path1;
  ASSERT_SOME(initializer.execute());

  const string path2 = os::getcwd() + "/.log2";
  initializer.flags.path = path2;
  ASSERT_SOME(initializer.execute());

  string servers = server->connectString();

  Log log1(2, path1, servers, NO_TIMEOUT, "/log/", None());
  Log log2(2, path2, servers, NO_TIMEOUT, "/log/", None());

  Log::Writer writer(&log2);

  Future<Option<Log::Position>> start = writer.start();

  AWAIT_READY(start);
  ASSERT_SOME(start.get());

  Future<Option<Log::Position>> position = writer.append("hello world");

  AWAIT_READY(position);
  ASSERT_SOME(position.get());

  Log::Reader reader(&log2);

  Future<list<Log::Entry>> entries =
    reader.read(position->get(), position->get());

  AWAIT_READY(entries);

  ASSERT_EQ(1u, entries->size());
  EXPECT_EQ(position->get(), entries->front().position);
  EXPECT_EQ("hello world", entries->front().data);
}


TEST_F(LogZooKeeperTest, LostZooKeeper)
{
  const string path = os::getcwd() + "/.log";
  const string servers = server->connectString();

  // We reply on auto-initialization to initialize the log.
  Log log(1, path, servers, NO_TIMEOUT, "/log/", None(), true);

  Log::Writer writer(&log);

  Future<Option<Log::Position>> start = writer.start();

  AWAIT_READY(start);
  ASSERT_SOME(start.get());

  // Shutdown ZooKeeper network.
  server->shutdownNetwork();

  // We should still be able to append as the local replica is in the
  // base set of the ZooKeeper network.
  Future<Option<Log::Position>> position = writer.append("hello world");

  AWAIT_READY(position);
  ASSERT_SOME(position.get());

  Log::Reader reader(&log);

  Future<list<Log::Entry>> entries =
    reader.read(position->get(), position->get());

  AWAIT_READY(entries);

  ASSERT_EQ(1u, entries->size());
  EXPECT_EQ(position->get(), entries->front().position);
  EXPECT_EQ("hello world", entries->front().data);
}
#endif // MESOS_HAS_JAVA


TEST_F(CoordinatorTest, RacingElect) {}

TEST_F(CoordinatorTest, FillNoQuorum) {}

TEST_F(CoordinatorTest, FillInconsistent) {}

TEST_F(CoordinatorTest, LearnedOnOneReplica_NotLearnedOnAnother) {}

TEST_F(CoordinatorTest,
       LearnedOnOneReplica_NotLearnedOnAnother_AnotherFailsAndRecovers) {}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
