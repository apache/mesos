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

#include <list>
#include <set>
#include <string>

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
#include <stout/try.hpp>

#include "log/catchup.hpp"
#include "log/coordinator.hpp"
#include "log/log.hpp"
#include "log/network.hpp"
#include "log/recover.hpp"
#include "log/replica.hpp"
#include "log/tool/initialize.hpp"

#include "tests/environment.hpp"
#include "tests/utils.hpp"

#ifdef MESOS_HAS_JAVA
#include "tests/zookeeper.hpp"
#endif

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::log;
using namespace mesos::internal::tests;

using namespace process;

using std::list;
using std::set;
using std::string;
using std::vector;

using testing::_;
using testing::Eq;
using testing::Return;


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


class ReplicaTest : public TemporaryDirectoryTest
{
protected:
  // For initializing the log.
  tool::Initialize initializer;
};


TEST_F(ReplicaTest, Promise)
{
  const string path = os::getcwd() + "/.log";
  initializer.flags.path = path;
  initializer.execute();

  Replica replica(path);

  PromiseRequest request;
  PromiseResponse response;
  Future<PromiseResponse> future;

  request.set_proposal(2);

  future = protocol::promise(replica.pid(), request);

  AWAIT_READY(future);

  response = future.get();
  EXPECT_TRUE(response.okay());
  EXPECT_EQ(2u, response.proposal());
  EXPECT_TRUE(response.has_position());
  EXPECT_EQ(0u, response.position());
  EXPECT_FALSE(response.has_action());

  request.set_proposal(1);

  future = protocol::promise(replica.pid(), request);

  AWAIT_READY(future);

  response = future.get();
  EXPECT_FALSE(response.okay());
  EXPECT_EQ(2u, response.proposal()); // Highest proposal seen so far.
  EXPECT_FALSE(response.has_position());
  EXPECT_FALSE(response.has_action());

  request.set_proposal(3);

  future = protocol::promise(replica.pid(), request);

  AWAIT_READY(future);

  response = future.get();
  EXPECT_TRUE(response.okay());
  EXPECT_EQ(3u, response.proposal());
  EXPECT_TRUE(response.has_position());
  EXPECT_EQ(0u, response.position());
  EXPECT_FALSE(response.has_action());
}


TEST_F(ReplicaTest, Append)
{
  const string path = os::getcwd() + "/.log";
  initializer.flags.path = path;
  initializer.execute();

  Replica replica(path);

  const uint64_t proposal = 1;

  PromiseRequest request1;
  request1.set_proposal(proposal);

  Future<PromiseResponse> future1 =
    protocol::promise(replica.pid(), request1);

  AWAIT_READY(future1);

  PromiseResponse response1 = future1.get();
  EXPECT_TRUE(response1.okay());
  EXPECT_EQ(proposal, response1.proposal());
  EXPECT_TRUE(response1.has_position());
  EXPECT_EQ(0u, response1.position());
  EXPECT_FALSE(response1.has_action());

  WriteRequest request2;
  request2.set_proposal(proposal);
  request2.set_position(1);
  request2.set_type(Action::APPEND);
  request2.mutable_append()->set_bytes("hello world");

  Future<WriteResponse> future2 =
    protocol::write(replica.pid(), request2);

  AWAIT_READY(future2);

  WriteResponse response2 = future2.get();
  EXPECT_TRUE(response2.okay());
  EXPECT_EQ(proposal, response2.proposal());
  EXPECT_EQ(1u, response2.position());

  Future<list<Action> > actions = replica.read(1, 1);

  AWAIT_READY(actions);
  ASSERT_EQ(1u, actions.get().size());

  Action action = actions.get().front();
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
  initializer.execute();

  Replica replica1(path);

  const uint64_t proposal= 1;

  PromiseRequest request1;
  request1.set_proposal(proposal);

  Future<PromiseResponse> future1 =
    protocol::promise(replica1.pid(), request1);

  AWAIT_READY(future1);

  PromiseResponse response1 = future1.get();
  EXPECT_TRUE(response1.okay());
  EXPECT_EQ(proposal, response1.proposal());
  EXPECT_TRUE(response1.has_position());
  EXPECT_EQ(0u, response1.position());
  EXPECT_FALSE(response1.has_action());

  WriteRequest request2;
  request2.set_proposal(proposal);
  request2.set_position(1);
  request2.set_type(Action::APPEND);
  request2.mutable_append()->set_bytes("hello world");

  Future<WriteResponse> future2 =
    protocol::write(replica1.pid(), request2);

  AWAIT_READY(future2);

  WriteResponse response2 = future2.get();
  EXPECT_TRUE(response2.okay());
  EXPECT_EQ(proposal, response2.proposal());
  EXPECT_EQ(1u, response2.position());

  Future<list<Action> > actions1 = replica1.read(1, 1);

  AWAIT_READY(actions1);
  ASSERT_EQ(1u, actions1.get().size());

  {
    Action action = actions1.get().front();
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

  Replica replica2(path);

  Future<list<Action> > actions2 = replica2.read(1, 1);

  AWAIT_READY(actions2);
  ASSERT_EQ(1u, actions2.get().size());

  {
    Action action = actions2.get().front();
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


// This test verifies that a non-VOTING replica does not reply to
// promise or write requests.
TEST_F(ReplicaTest, NonVoting)
{
  const string path = os::getcwd() + "/.log";

  Replica replica(path);

  PromiseRequest promiseRequest;
  promiseRequest.set_proposal(2);

  Future<PromiseResponse> promiseResponse =
    protocol::promise(replica.pid(), promiseRequest);

  // Flush the event queue to make sure that if the replica could
  // reply to the promise request, the future 'promiseResponse' would
  // be satisfied before the pending check below.
  Clock::pause();
  Clock::settle();
  Clock::resume();

  EXPECT_TRUE(promiseResponse.isPending());

  WriteRequest writeRequest;
  writeRequest.set_proposal(3);
  writeRequest.set_position(1);
  writeRequest.set_type(Action::APPEND);
  writeRequest.mutable_append()->set_bytes("hello world");

  Future<WriteResponse> writeResponse =
    protocol::write(replica.pid(), writeRequest);

  // Flush the event queue to make sure that if the replica could
  // reply to the write request, the future 'writeResponse' would be
  // satisfied before the pending check below.
  Clock::pause();
  Clock::settle();
  Clock::resume();

  EXPECT_TRUE(writeResponse.isPending());
}


class CoordinatorTest : public TemporaryDirectoryTest
{
protected:
  // For initializing the log.
  tool::Initialize initializer;
};


TEST_F(CoordinatorTest, Elect)
{
  const string path1 = os::getcwd() + "/.log1";
  initializer.flags.path = path1;
  initializer.execute();

  const string path2 = os::getcwd() + "/.log2";
  initializer.flags.path = path2;
  initializer.execute();

  Shared<Replica> replica1(new Replica(path1));
  Shared<Replica> replica2(new Replica(path2));

  set<UPID> pids;
  pids.insert(replica1->pid());
  pids.insert(replica2->pid());

  Shared<Network> network(new Network(pids));

  Coordinator coord(2, replica1, network);

  {
    Future<Option<uint64_t> > electing = coord.elect();
    AWAIT_READY_FOR(electing, Seconds(10));
    ASSERT_SOME(electing.get());
    EXPECT_EQ(0u, electing.get().get());
  }

  {
    Future<list<Action> > actions = replica1->read(0, 0);
    AWAIT_READY(actions);
    ASSERT_EQ(1u, actions.get().size());
    EXPECT_EQ(0u, actions.get().front().position());
    ASSERT_TRUE(actions.get().front().has_type());
    ASSERT_EQ(Action::NOP, actions.get().front().type());
  }
}


TEST_F(CoordinatorTest, AppendRead)
{
  const string path1 = os::getcwd() + "/.log1";
  initializer.flags.path = path1;
  initializer.execute();

  const string path2 = os::getcwd() + "/.log2";
  initializer.flags.path = path2;
  initializer.execute();

  Shared<Replica> replica1(new Replica(path1));
  Shared<Replica> replica2(new Replica(path2));

  set<UPID> pids;
  pids.insert(replica1->pid());
  pids.insert(replica2->pid());

  Shared<Network> network(new Network(pids));

  Coordinator coord(2, replica1, network);

  {
    Future<Option<uint64_t> > electing = coord.elect();
    AWAIT_READY_FOR(electing, Seconds(10));
    ASSERT_SOME(electing.get());
    EXPECT_EQ(0u, electing.get().get());
  }

  uint64_t position;

  {
    Future<uint64_t> appending = coord.append("hello world");
    AWAIT_READY_FOR(appending, Seconds(10));
    position = appending.get();
    EXPECT_EQ(1u, position);
  }

  {
    Future<list<Action> > actions = replica1->read(position, position);
    AWAIT_READY(actions);
    ASSERT_EQ(1u, actions.get().size());
    EXPECT_EQ(position, actions.get().front().position());
    ASSERT_TRUE(actions.get().front().has_type());
    ASSERT_EQ(Action::APPEND, actions.get().front().type());
    EXPECT_EQ("hello world", actions.get().front().append().bytes());
  }
}


TEST_F(CoordinatorTest, AppendReadError)
{
  const string path1 = os::getcwd() + "/.log1";
  initializer.flags.path = path1;
  initializer.execute();

  const string path2 = os::getcwd() + "/.log2";
  initializer.flags.path = path2;
  initializer.execute();

  Shared<Replica> replica1(new Replica(path1));
  Shared<Replica> replica2(new Replica(path2));

  set<UPID> pids;
  pids.insert(replica1->pid());
  pids.insert(replica2->pid());

  Shared<Network> network(new Network(pids));

  Coordinator coord(2, replica1, network);

  {
    Future<Option<uint64_t> > electing = coord.elect();
    AWAIT_READY_FOR(electing, Seconds(10));
    ASSERT_SOME(electing.get());
    EXPECT_EQ(0u, electing.get().get());
  }

  uint64_t position;

  {
    Future<uint64_t> appending = coord.append("hello world");
    AWAIT_READY_FOR(appending, Seconds(10));
    position = appending.get();
    EXPECT_EQ(1u, position);
  }

  {
    position += 1;
    Future<list<Action> > actions = replica1->read(position, position);
    AWAIT_FAILED(actions);
    EXPECT_EQ("Bad read range (past end of log)", actions.failure());
  }
}


TEST_F(CoordinatorTest, ElectNoQuorum)
{
  const string path = os::getcwd() + "/.log";
  initializer.flags.path = path;
  initializer.execute();

  Shared<Replica> replica(new Replica(path));

  set<UPID> pids;
  pids.insert(replica->pid());

  Shared<Network> network(new Network(pids));

  Coordinator coord(2, replica, network);

  Clock::pause();

  Future<Option<uint64_t> > electing = coord.elect();

  Clock::advance(Seconds(10));
  Clock::settle();

  EXPECT_TRUE(electing.isPending());

  Clock::resume();
}


TEST_F(CoordinatorTest, AppendNoQuorum)
{
  const string path1 = os::getcwd() + "/.log1";
  initializer.flags.path = path1;
  initializer.execute();

  const string path2 = os::getcwd() + "/.log2";
  initializer.flags.path = path2;
  initializer.execute();

  Shared<Replica> replica1(new Replica(path1));
  Shared<Replica> replica2(new Replica(path2));

  set<UPID> pids;
  pids.insert(replica1->pid());
  pids.insert(replica2->pid());

  Shared<Network> network(new Network(pids));

  Coordinator coord(2, replica1, network);

  {
    Future<Option<uint64_t> > electing = coord.elect();
    AWAIT_READY_FOR(electing, Seconds(10));
    ASSERT_SOME(electing.get());
    EXPECT_EQ(0u, electing.get().get());
  }

  process::terminate(replica2->pid());
  process::wait(replica2->pid());
  replica2.reset();

  Clock::pause();

  Future<uint64_t> appending = coord.append("hello world");

  Clock::advance(Seconds(10));
  Clock::settle();

  EXPECT_TRUE(appending.isPending());

  Clock::resume();
}


TEST_F(CoordinatorTest, Failover)
{
  const string path1 = os::getcwd() + "/.log1";
  initializer.flags.path = path1;
  initializer.execute();

  const string path2 = os::getcwd() + "/.log2";
  initializer.flags.path = path2;
  initializer.execute();

  Shared<Replica> replica1(new Replica(path1));
  Shared<Replica> replica2(new Replica(path2));

  set<UPID> pids;
  pids.insert(replica1->pid());
  pids.insert(replica2->pid());

  Shared<Network> network1(new Network(pids));

  Coordinator coord1(2, replica1, network1);

  {
    Future<Option<uint64_t> > electing = coord1.elect();
    AWAIT_READY_FOR(electing, Seconds(10));
    ASSERT_SOME(electing.get());
    EXPECT_EQ(0u, electing.get().get());
  }

  uint64_t position;

  {
    Future<uint64_t> appending = coord1.append("hello world");
    AWAIT_READY_FOR(appending, Seconds(10));
    position = appending.get();
    EXPECT_EQ(1u, position);
  }

  Shared<Network> network2(new Network(pids));

  Coordinator coord2(2, replica2, network2);

  {
    Future<Option<uint64_t> > electing = coord2.elect();
    AWAIT_READY_FOR(electing, Seconds(10));
    ASSERT_SOME(electing.get());
    EXPECT_EQ(position, electing.get().get());
  }

  {
    Future<list<Action> > actions = replica2->read(position, position);
    AWAIT_READY(actions);
    ASSERT_EQ(1u, actions.get().size());
    EXPECT_EQ(position, actions.get().front().position());
    ASSERT_TRUE(actions.get().front().has_type());
    ASSERT_EQ(Action::APPEND, actions.get().front().type());
    EXPECT_EQ("hello world", actions.get().front().append().bytes());
  }
}


TEST_F(CoordinatorTest, Demoted)
{
  const string path1 = os::getcwd() + "/.log1";
  initializer.flags.path = path1;
  initializer.execute();

  const string path2 = os::getcwd() + "/.log2";
  initializer.flags.path = path2;
  initializer.execute();

  Shared<Replica> replica1(new Replica(path1));
  Shared<Replica> replica2(new Replica(path2));

  set<UPID> pids;
  pids.insert(replica1->pid());
  pids.insert(replica2->pid());

  Shared<Network> network1(new Network(pids));

  Coordinator coord1(2, replica1, network1);

  {
    Future<Option<uint64_t> > electing = coord1.elect();
    AWAIT_READY_FOR(electing, Seconds(10));
    ASSERT_SOME(electing.get());
    EXPECT_EQ(0u, electing.get().get());
  }

  uint64_t position;

  {
    Future<uint64_t> appending = coord1.append("hello world");
    AWAIT_READY_FOR(appending, Seconds(10));
    position = appending.get();
    EXPECT_EQ(1u, position);
  }

  Shared<Network> network2(new Network(pids));

  Coordinator coord2(2, replica2, network2);

  {
    Future<Option<uint64_t> > electing = coord2.elect();
    AWAIT_READY_FOR(electing, Seconds(10));
    ASSERT_SOME(electing.get());
    EXPECT_EQ(position, electing.get().get());
  }

  {
    Future<uint64_t> appending = coord1.append("hello moto");
    AWAIT_FAILED(appending);
    EXPECT_EQ("Coordinator demoted", appending.failure());
  }

  {
    Future<uint64_t> appending = coord2.append("hello hello");
    AWAIT_READY_FOR(appending, Seconds(10));
    position = appending.get();
    EXPECT_EQ(2u, position);
  }

  {
    Future<list<Action> > actions = replica2->read(position, position);
    AWAIT_READY(actions);
    ASSERT_EQ(1u, actions.get().size());
    EXPECT_EQ(position, actions.get().front().position());
    ASSERT_TRUE(actions.get().front().has_type());
    ASSERT_EQ(Action::APPEND, actions.get().front().type());
    EXPECT_EQ("hello hello", actions.get().front().append().bytes());
  }
}


TEST_F(CoordinatorTest, Fill)
{
  const string path1 = os::getcwd() + "/.log1";
  initializer.flags.path = path1;
  initializer.execute();

  const string path2 = os::getcwd() + "/.log2";
  initializer.flags.path = path2;
  initializer.execute();

  const string path3 = os::getcwd() + "/.log3";
  initializer.flags.path = path3;
  initializer.execute();

  Shared<Replica> replica1(new Replica(path1));
  Shared<Replica> replica2(new Replica(path2));

  set<UPID> pids;
  pids.insert(replica1->pid());
  pids.insert(replica2->pid());

  Shared<Network> network1(new Network(pids));

  Coordinator coord1(2, replica1, network1);

  {
    Future<Option<uint64_t> > electing = coord1.elect();
    AWAIT_READY_FOR(electing, Seconds(10));
    ASSERT_SOME(electing.get());
    EXPECT_EQ(0u, electing.get().get());
  }

  uint64_t position;

  {
    Future<uint64_t> appending = coord1.append("hello world");
    AWAIT_READY_FOR(appending, Seconds(10));
    position = appending.get();
    EXPECT_EQ(1u, position);
  }

  Shared<Replica> replica3(new Replica(path3));

  pids.clear();
  pids.insert(replica2->pid());
  pids.insert(replica3->pid());

  Shared<Network> network2(new Network(pids));

  Coordinator coord2(2, replica3, network2);

  {
    Future<Option<uint64_t> > electing = coord2.elect();
    AWAIT_READY_FOR(electing, Seconds(10));
    ASSERT_NONE(electing.get());

    electing = coord2.elect();
    AWAIT_READY_FOR(electing, Seconds(10));
    ASSERT_SOME(electing.get());
    EXPECT_EQ(position, electing.get().get());
  }

  {
    Future<list<Action> > actions = replica3->read(position, position);
    AWAIT_READY(actions);
    ASSERT_EQ(1u, actions.get().size());
    EXPECT_EQ(position, actions.get().front().position());
    ASSERT_TRUE(actions.get().front().has_type());
    ASSERT_EQ(Action::APPEND, actions.get().front().type());
    EXPECT_EQ("hello world", actions.get().front().append().bytes());
  }
}


TEST_F(CoordinatorTest, NotLearnedFill)
{
  const string path1 = os::getcwd() + "/.log1";
  initializer.flags.path = path1;
  initializer.execute();

  const string path2 = os::getcwd() + "/.log2";
  initializer.flags.path = path2;
  initializer.execute();

  const string path3 = os::getcwd() + "/.log3";
  initializer.flags.path = path3;
  initializer.execute();

  Shared<Replica> replica1(new Replica(path1));
  Shared<Replica> replica2(new Replica(path2));

  // Drop messages here in order to obtain the pid of replica2. We
  // only want to drop learned message sent to replica2.
  DROP_MESSAGES(Eq(LearnedMessage().GetTypeName()), _, Eq(replica2->pid()));

  set<UPID> pids;
  pids.insert(replica1->pid());
  pids.insert(replica2->pid());

  Shared<Network> network1(new Network(pids));

  Coordinator coord1(2, replica1, network1);

  {
    Future<Option<uint64_t> > electing = coord1.elect();
    AWAIT_READY_FOR(electing, Seconds(10));
    ASSERT_SOME(electing.get());
    EXPECT_EQ(0u, electing.get().get());
  }

  uint64_t position;

  {
    Future<uint64_t> appending = coord1.append("hello world");
    AWAIT_READY_FOR(appending, Seconds(10));
    position = appending.get();
    EXPECT_EQ(1u, position);
  }

  Shared<Replica> replica3(new Replica(path3));

  pids.clear();
  pids.insert(replica2->pid());
  pids.insert(replica3->pid());

  Shared<Network> network2(new Network(pids));

  Coordinator coord2(2, replica3, network2);

  {
    Future<Option<uint64_t> > electing = coord2.elect();
    AWAIT_READY_FOR(electing, Seconds(10));
    ASSERT_NONE(electing.get());

    electing = coord2.elect();
    AWAIT_READY_FOR(electing, Seconds(10));
    ASSERT_SOME(electing.get());
    EXPECT_EQ(position, electing.get().get());
  }

  {
    Future<list<Action> > actions = replica3->read(position, position);
    AWAIT_READY(actions);
    ASSERT_EQ(1u, actions.get().size());
    EXPECT_EQ(position, actions.get().front().position());
    ASSERT_TRUE(actions.get().front().has_type());
    ASSERT_EQ(Action::APPEND, actions.get().front().type());
    EXPECT_EQ("hello world", actions.get().front().append().bytes());
  }
}


TEST_F(CoordinatorTest, MultipleAppends)
{
  const string path1 = os::getcwd() + "/.log1";
  initializer.flags.path = path1;
  initializer.execute();

  const string path2 = os::getcwd() + "/.log2";
  initializer.flags.path = path2;
  initializer.execute();

  Shared<Replica> replica1(new Replica(path1));
  Shared<Replica> replica2(new Replica(path2));

  set<UPID> pids;
  pids.insert(replica1->pid());
  pids.insert(replica2->pid());

  Shared<Network> network(new Network(pids));

  Coordinator coord(2, replica1, network);

  {
    Future<Option<uint64_t> > electing = coord.elect();
    AWAIT_READY_FOR(electing, Seconds(10));
    ASSERT_SOME(electing.get());
    EXPECT_EQ(0u, electing.get().get());
  }

  for (uint64_t position = 1; position <= 10; position++) {
    Future<uint64_t> appending = coord.append(stringify(position));
    AWAIT_READY_FOR(appending, Seconds(10));
    EXPECT_EQ(position, appending.get());
  }

  {
    Future<list<Action> > actions = replica1->read(1, 10);
    AWAIT_READY(actions);
    EXPECT_EQ(10u, actions.get().size());
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
  initializer.execute();

  const string path2 = os::getcwd() + "/.log2";
  initializer.flags.path = path2;
  initializer.execute();

  const string path3 = os::getcwd() + "/.log3";
  initializer.flags.path = path3;
  initializer.execute();

  Shared<Replica> replica1(new Replica(path1));
  Shared<Replica> replica2(new Replica(path2));

  // Drop messages here in order to obtain the pid of replica2. We
  // only want to drop learned message sent to replica2.
  DROP_MESSAGES(Eq(LearnedMessage().GetTypeName()), _, Eq(replica2->pid()));

  set<UPID> pids;
  pids.insert(replica1->pid());
  pids.insert(replica2->pid());

  Shared<Network> network1(new Network(pids));

  Coordinator coord1(2, replica1, network1);

  {
    Future<Option<uint64_t> > electing = coord1.elect();
    AWAIT_READY_FOR(electing, Seconds(10));
    ASSERT_SOME(electing.get());
    EXPECT_EQ(0u, electing.get().get());
  }

  for (uint64_t position = 1; position <= 10; position++) {
    Future<uint64_t> appending = coord1.append(stringify(position));
    AWAIT_READY_FOR(appending, Seconds(10));
    EXPECT_EQ(position, appending.get());
  }

  Shared<Replica> replica3(new Replica(path3));

  pids.clear();
  pids.insert(replica2->pid());
  pids.insert(replica3->pid());

  Shared<Network> network2(new Network(pids));

  Coordinator coord2(2, replica3, network2);

  {
    Future<Option<uint64_t> > electing = coord2.elect();
    AWAIT_READY_FOR(electing, Seconds(10));
    ASSERT_NONE(electing.get());

    electing = coord2.elect();
    AWAIT_READY_FOR(electing, Seconds(10));
    ASSERT_SOME(electing.get());
    EXPECT_EQ(10u, electing.get().get());
  }

  {
    Future<list<Action> > actions = replica3->read(1, 10);
    AWAIT_READY(actions);
    EXPECT_EQ(10u, actions.get().size());
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
  initializer.execute();

  const string path2 = os::getcwd() + "/.log2";
  initializer.flags.path = path2;
  initializer.execute();

  Shared<Replica> replica1(new Replica(path1));
  Shared<Replica> replica2(new Replica(path2));

  set<UPID> pids;
  pids.insert(replica1->pid());
  pids.insert(replica2->pid());

  Shared<Network> network(new Network(pids));

  Coordinator coord(2, replica1, network);

  {
    Future<Option<uint64_t> > electing = coord.elect();
    AWAIT_READY_FOR(electing, Seconds(10));
    ASSERT_SOME(electing.get());
    EXPECT_EQ(0u, electing.get().get());
  }

  for (uint64_t position = 1; position <= 10; position++) {
    Future<uint64_t> appending = coord.append(stringify(position));
    AWAIT_READY_FOR(appending, Seconds(10));
    EXPECT_EQ(position, appending.get());
  }

  {
    Future<uint64_t> truncating = coord.truncate(7);
    AWAIT_READY_FOR(truncating, Seconds(10));
    EXPECT_EQ(11u, truncating.get());
  }

  {
    Future<list<Action> > actions = replica1->read(6, 10);
    AWAIT_FAILED(actions);
    EXPECT_EQ("Bad read range (truncated position)", actions.failure());
  }

  {
    Future<list<Action> > actions = replica1->read(7, 10);
    AWAIT_READY(actions);
    EXPECT_EQ(4u, actions.get().size());
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
  initializer.execute();

  const string path2 = os::getcwd() + "/.log2";
  initializer.flags.path = path2;
  initializer.execute();

  const string path3 = os::getcwd() + "/.log3";
  initializer.flags.path = path3;
  initializer.execute();

  Shared<Replica> replica1(new Replica(path1));
  Shared<Replica> replica2(new Replica(path2));

  // Drop messages here in order to obtain the pid of replica2. We
  // only want to drop learned message sent to replica2.
  DROP_MESSAGES(Eq(LearnedMessage().GetTypeName()), _, Eq(replica2->pid()));

  set<UPID> pids;
  pids.insert(replica1->pid());
  pids.insert(replica2->pid());

  Shared<Network> network1(new Network(pids));

  Coordinator coord1(2, replica1, network1);

  {
    Future<Option<uint64_t> > electing = coord1.elect();
    AWAIT_READY_FOR(electing, Seconds(10));
    ASSERT_SOME(electing.get());
    EXPECT_EQ(0u, electing.get().get());
  }

  for (uint64_t position = 1; position <= 10; position++) {
    Future<uint64_t> appending = coord1.append(stringify(position));
    AWAIT_READY_FOR(appending, Seconds(10));
    EXPECT_EQ(position, appending.get());
  }

  {
    Future<uint64_t> truncating = coord1.truncate(7);
    AWAIT_READY_FOR(truncating, Seconds(10));
    EXPECT_EQ(11u, truncating.get());
  }

  Shared<Replica> replica3(new Replica(path3));

  pids.clear();
  pids.insert(replica2->pid());
  pids.insert(replica3->pid());

  Shared<Network> network2(new Network(pids));

  Coordinator coord2(2, replica3, network2);

  {
    Future<Option<uint64_t> > electing = coord2.elect();
    AWAIT_READY_FOR(electing, Seconds(10));
    ASSERT_NONE(electing.get());

    electing = coord2.elect();
    AWAIT_READY_FOR(electing, Seconds(10));
    ASSERT_SOME(electing.get());
    EXPECT_EQ(11u, electing.get().get());
  }

  {
    Future<list<Action> > actions = replica3->read(6, 10);
    AWAIT_FAILED(actions);
    EXPECT_EQ("Bad read range (truncated position)", actions.failure());
  }

  {
    Future<list<Action> > actions = replica3->read(7, 10);
    AWAIT_READY(actions);
    EXPECT_EQ(4u, actions.get().size());
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
  initializer.execute();

  const string path2 = os::getcwd() + "/.log2";
  initializer.flags.path = path2;
  initializer.execute();

  const string path3 = os::getcwd() + "/.log3";
  initializer.flags.path = path3;
  initializer.execute();

  Shared<Replica> replica1(new Replica(path1));
  Shared<Replica> replica2(new Replica(path2));

  set<UPID> pids;
  pids.insert(replica1->pid());
  pids.insert(replica2->pid());

  Shared<Network> network1(new Network(pids));

  Coordinator coord1(2, replica1, network1);

  {
    Future<Option<uint64_t> > electing = coord1.elect();
    AWAIT_READY_FOR(electing, Seconds(10));
    ASSERT_SOME(electing.get());
    EXPECT_EQ(0u, electing.get().get());
  }

  for (uint64_t position = 1; position <= 10; position++) {
    Future<uint64_t> appending = coord1.append(stringify(position));
    AWAIT_READY_FOR(appending, Seconds(10));
    EXPECT_EQ(position, appending.get());
  }

  {
    Future<uint64_t> truncating = coord1.truncate(7);
    AWAIT_READY_FOR(truncating, Seconds(10));
    EXPECT_EQ(11u, truncating.get());
  }

  Shared<Replica> replica3(new Replica(path3));

  pids.clear();
  pids.insert(replica2->pid());
  pids.insert(replica3->pid());

  Shared<Network> network2(new Network(pids));

  Coordinator coord2(2, replica3, network2);

  {
    Future<Option<uint64_t> > electing = coord2.elect();
    AWAIT_READY_FOR(electing, Seconds(10));
    ASSERT_NONE(electing.get());

    electing = coord2.elect();
    AWAIT_READY_FOR(electing, Seconds(10));
    ASSERT_SOME(electing.get());
    EXPECT_EQ(11u, electing.get().get());
  }

  {
    Future<list<Action> > actions = replica3->read(6, 10);
    AWAIT_FAILED(actions);
    EXPECT_EQ("Bad read range (truncated position)", actions.failure());
  }

  {
    Future<list<Action> > actions = replica3->read(7, 10);
    AWAIT_READY(actions);
    EXPECT_EQ(4u, actions.get().size());
    foreach (const Action& action, actions.get()) {
      ASSERT_TRUE(action.has_type());
      ASSERT_EQ(Action::APPEND, action.type());
      EXPECT_EQ(stringify(action.position()), action.append().bytes());
    }
  }
}


class RecoverTest : public TemporaryDirectoryTest
{
protected:
  // For initializing the log.
  tool::Initialize initializer;
};


// Two logs both need recovery compete with each other.
TEST_F(RecoverTest, RacingCatchup)
{
  const string path1 = os::getcwd() + "/.log1";
  initializer.flags.path = path1;
  initializer.execute();

  const string path2 = os::getcwd() + "/.log2";
  initializer.flags.path = path2;
  initializer.execute();

  const string path3 = os::getcwd() + "/.log3";
  initializer.flags.path = path3;
  initializer.execute();

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
    Future<Option<uint64_t> > electing = coord1.elect();
    AWAIT_READY_FOR(electing, Seconds(10));
    ASSERT_SOME(electing.get());
    EXPECT_EQ(0u, electing.get().get());
  }

  for (uint64_t position = 1; position <= 10; position++) {
    Future<uint64_t> appending = coord1.append(stringify(position));
    AWAIT_READY_FOR(appending, Seconds(10));
    EXPECT_EQ(position, appending.get());
  }

  // Two replicas both want to recover.
  Owned<Replica> replica4(new Replica(path4));
  Owned<Replica> replica5(new Replica(path5));

  pids.insert(replica4->pid());
  pids.insert(replica5->pid());

  Shared<Network> network2(new Network(pids));

  Future<Owned<Replica> > recovering4 = recover(3, replica4, network2);
  Future<Owned<Replica> > recovering5 = recover(3, replica5, network2);

  // Wait until recovery is done.
  AWAIT_READY(recovering4);
  AWAIT_READY(recovering5);

  Shared<Replica> shared4 = recovering4.get().share();
  Coordinator coord2(3, shared4, network2);

  {
    Future<Option<uint64_t> > electing = coord2.elect();
    AWAIT_READY_FOR(electing, Seconds(10));
    ASSERT_NONE(electing.get());

    electing = coord2.elect();
    AWAIT_READY_FOR(electing, Seconds(10));
    ASSERT_SOME(electing.get());
    EXPECT_EQ(10u, electing.get().get());
  }

  {
    Future<list<Action> > actions = shared4->read(1, 10);
    AWAIT_READY(actions);
    EXPECT_EQ(10u, actions.get().size());
    foreach (const Action& action, actions.get()) {
      ASSERT_TRUE(action.has_type());
      ASSERT_EQ(Action::APPEND, action.type());
      EXPECT_EQ(stringify(action.position()), action.append().bytes());
    }
  }

  {
    Future<uint64_t> appending = coord2.append("hello hello");
    AWAIT_READY_FOR(appending, Seconds(10));
    EXPECT_EQ(11u, appending.get());
  }

  {
    Future<list<Action> > actions = shared4->read(11u, 11u);
    AWAIT_READY(actions);
    ASSERT_EQ(1u, actions.get().size());
    EXPECT_EQ(11u, actions.get().front().position());
    ASSERT_TRUE(actions.get().front().has_type());
    ASSERT_EQ(Action::APPEND, actions.get().front().type());
    EXPECT_EQ("hello hello", actions.get().front().append().bytes());
  }
}


TEST_F(RecoverTest, CatchupRetry)
{
  const string path1 = os::getcwd() + "/.log1";
  initializer.flags.path = path1;
  initializer.execute();

  const string path2 = os::getcwd() + "/.log2";
  initializer.flags.path = path2;
  initializer.execute();

  const string path3 = os::getcwd() + "/.log3";

  Shared<Replica> replica1(new Replica(path1));
  Shared<Replica> replica2(new Replica(path2));

  // Make sure replica2 does not receive learned messages.
  DROP_MESSAGES(Eq(LearnedMessage().GetTypeName()), _, Eq(replica2->pid()));

  set<UPID> pids;
  pids.insert(replica1->pid());
  pids.insert(replica2->pid());

  Shared<Network> network1(new Network(pids));

  Coordinator coord(2, replica1, network1);

  {
    Future<Option<uint64_t> > electing = coord.elect();
    AWAIT_READY_FOR(electing, Seconds(10));
    ASSERT_SOME(electing.get());
    EXPECT_EQ(0u, electing.get().get());
  }

  vector<uint64_t> positions;

  for (uint64_t position = 1; position <= 10; position++) {
    Future<uint64_t> appending = coord.append(stringify(position));
    AWAIT_READY_FOR(appending, Seconds(10));
    EXPECT_EQ(position, appending.get());
    positions.push_back(position);
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
  DROP_MESSAGE(Eq(PromiseRequest().GetTypeName()), _, Eq(replica1->pid()));

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


class LogTest : public TemporaryDirectoryTest
{
protected:
  // For initializing the log.
  tool::Initialize initializer;
};


TEST_F(LogTest, WriteRead)
{
  const string path1 = os::getcwd() + "/.log1";
  initializer.flags.path = path1;
  initializer.execute();

  const string path2 = os::getcwd() + "/.log2";
  initializer.flags.path = path2;
  initializer.execute();

  Replica replica1(path1);

  set<UPID> pids;
  pids.insert(replica1.pid());

  Log log(2, path2, pids);

  Log::Writer writer(&log, Seconds(10));

  Result<Log::Position> position =
    writer.append("hello world", Timeout::in(Seconds(10)));

  ASSERT_SOME(position);

  Log::Reader reader(&log);

  Result<list<Log::Entry> > entries =
    reader.read(position.get(), position.get(), Timeout::in(Seconds(10)));

  ASSERT_SOME(entries);
  ASSERT_EQ(1u, entries.get().size());
  EXPECT_EQ(position.get(), entries.get().front().position);
  EXPECT_EQ("hello world", entries.get().front().data);
}


TEST_F(LogTest, Position)
{
  const string path1 = os::getcwd() + "/.log1";
  initializer.flags.path = path1;
  initializer.execute();

  const string path2 = os::getcwd() + "/.log2";
  initializer.flags.path = path2;
  initializer.execute();

  Replica replica1(path1);

  set<UPID> pids;
  pids.insert(replica1.pid());

  Log log(2, path2, pids);

  Log::Writer writer(&log, Seconds(10));

  Result<Log::Position> position =
    writer.append("hello world", Timeout::in(Seconds(10)));

  ASSERT_SOME(position);

  ASSERT_EQ(position.get(), log.position(position.get().identity()));
}


#ifdef MESOS_HAS_JAVA
// TODO(jieyu): We copy the code from TemporaryDirectoryTest here
// because we cannot inherit from two test fixtures. In this future,
// we need a way to compose multiple test fixtures together.
class LogZooKeeperTest : public ZooKeeperTest
{
protected:
  virtual void SetUp()
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
    ASSERT_TRUE(os::chdir(sandbox.get()))
      << "Failed to chdir into '" << sandbox.get() << "'";
  }

  virtual void TearDown()
  {
    // Return to previous working directory and cleanup the sandbox.
    ASSERT_TRUE(os::chdir(cwd));

    if (sandbox.isSome()) {
      ASSERT_SOME(os::rmdir(sandbox.get()));
    }
  }

  // For initializing the log.
  tool::Initialize initializer;

private:
  string cwd;
  Option<string> sandbox;
};


TEST_F(LogZooKeeperTest, WriteRead)
{
  const string path1 = os::getcwd() + "/.log1";
  initializer.flags.path = path1;
  initializer.execute();

  const string path2 = os::getcwd() + "/.log2";
  initializer.flags.path = path2;
  initializer.execute();

  string servers = server->connectString();

  Log log1(2, path1, servers, NO_TIMEOUT, "/log/", None());
  Log log2(2, path2, servers, NO_TIMEOUT, "/log/", None());

  Log::Writer writer(&log2, Seconds(10));

  Result<Log::Position> position =
    writer.append("hello world", Timeout::in(Seconds(10)));

  ASSERT_SOME(position);

  Log::Reader reader(&log2);

  Result<list<Log::Entry> > entries =
    reader.read(position.get(), position.get(), Timeout::in(Seconds(10)));

  ASSERT_SOME(entries);
  ASSERT_EQ(1u, entries.get().size());
  EXPECT_EQ(position.get(), entries.get().front().position);
  EXPECT_EQ("hello world", entries.get().front().data);
}
#endif // MESOS_HAS_JAVA


TEST_F(CoordinatorTest, RacingElect) {}

TEST_F(CoordinatorTest, FillNoQuorum) {}

TEST_F(CoordinatorTest, FillInconsistent) {}

TEST_F(CoordinatorTest, LearnedOnOneReplica_NotLearnedOnAnother) {}

TEST_F(CoordinatorTest,
       LearnedOnOneReplica_NotLearnedOnAnother_AnotherFailsAndRecovers) {}
