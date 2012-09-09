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

#include <set>
#include <string>

#include <process/future.hpp>
#include <process/protobuf.hpp>

#include <stout/option.hpp>
#include <stout/os.hpp>

#include "common/type_utils.hpp"

#include "log/coordinator.hpp"
#include "log/log.hpp"
#include "log/replica.hpp"

#include "messages/messages.hpp"

#include "tests/utils.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::log;
using namespace mesos::internal::test;

using namespace process;

using testing::_;
using testing::Eq;
using testing::Return;


TEST(ReplicaTest, Promise)
{
  const std::string path = os::getcwd() + "/.log";

  os::rmdir(path);

  Replica replica(path);

  PromiseRequest request;
  PromiseResponse response;
  Future<PromiseResponse> future;

  request.set_id(2);

  future = protocol::promise(replica.pid(), request);

  future.await(Seconds(2.0));
  ASSERT_TRUE(future.isReady());

  response = future.get();
  EXPECT_TRUE(response.okay());
  EXPECT_EQ(2u, response.id());
  EXPECT_TRUE(response.has_position());
  EXPECT_EQ(0u, response.position());
  EXPECT_FALSE(response.has_action());

  request.set_id(1);

  future = protocol::promise(replica.pid(), request);

  future.await(Seconds(2.0));
  ASSERT_TRUE(future.isReady());

  response = future.get();
  EXPECT_FALSE(response.okay());
  EXPECT_EQ(1u, response.id());
  EXPECT_FALSE(response.has_position());
  EXPECT_FALSE(response.has_action());

  request.set_id(3);

  future = protocol::promise(replica.pid(), request);

  future.await(Seconds(2.0));
  ASSERT_TRUE(future.isReady());

  response = future.get();
  EXPECT_TRUE(response.okay());
  EXPECT_EQ(3u, response.id());
  EXPECT_TRUE(response.has_position());
  EXPECT_EQ(0u, response.position());
  EXPECT_FALSE(response.has_action());

  os::rmdir(path);
}


TEST(ReplicaTest, Append)
{
  const std::string path = os::getcwd() + "/.log";

  os::rmdir(path);

  Replica replica(path);

  const uint64_t id = 1;

  PromiseRequest request1;
  request1.set_id(id);

  Future<PromiseResponse> future1 =
    protocol::promise(replica.pid(), request1);

  future1.await(Seconds(2.0));
  ASSERT_TRUE(future1.isReady());

  PromiseResponse response1 = future1.get();
  EXPECT_TRUE(response1.okay());
  EXPECT_EQ(id, response1.id());
  EXPECT_TRUE(response1.has_position());
  EXPECT_EQ(0u, response1.position());
  EXPECT_FALSE(response1.has_action());

  WriteRequest request2;
  request2.set_id(id);
  request2.set_position(1);
  request2.set_type(Action::APPEND);
  request2.mutable_append()->set_bytes("hello world");

  Future<WriteResponse> future2 =
    protocol::write(replica.pid(), request2);

  future2.await(Seconds(2.0));
  ASSERT_TRUE(future2.isReady());

  WriteResponse response2 = future2.get();
  EXPECT_TRUE(response2.okay());
  EXPECT_EQ(id, response2.id());
  EXPECT_EQ(1u, response2.position());

  Future<std::list<Action> > actions = replica.read(1, 1);
  ASSERT_TRUE(actions.await(Seconds(2.0)));
  ASSERT_TRUE(actions.isReady());
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

  os::rmdir(path);
}


TEST(ReplicaTest, Recover)
{
  const std::string path = os::getcwd() + "/.log";

  os::rmdir(path);

  Replica replica1(path);

  const uint64_t id = 1;

  PromiseRequest request1;
  request1.set_id(id);

  Future<PromiseResponse> future1 =
    protocol::promise(replica1.pid(), request1);

  future1.await(Seconds(2.0));
  ASSERT_TRUE(future1.isReady());

  PromiseResponse response1 = future1.get();
  EXPECT_TRUE(response1.okay());
  EXPECT_EQ(id, response1.id());
  EXPECT_TRUE(response1.has_position());
  EXPECT_EQ(0u, response1.position());
  EXPECT_FALSE(response1.has_action());

  WriteRequest request2;
  request2.set_id(id);
  request2.set_position(1);
  request2.set_type(Action::APPEND);
  request2.mutable_append()->set_bytes("hello world");

  Future<WriteResponse> future2 =
    protocol::write(replica1.pid(), request2);

  future2.await(Seconds(2.0));
  ASSERT_TRUE(future2.isReady());

  WriteResponse response2 = future2.get();
  EXPECT_TRUE(response2.okay());
  EXPECT_EQ(id, response2.id());
  EXPECT_EQ(1u, response2.position());

  Future<std::list<Action> > actions1 = replica1.read(1, 1);
  ASSERT_TRUE(actions1.await(Seconds(2.0)));
  ASSERT_TRUE(actions1.isReady());
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

  Future<std::list<Action> > actions2 = replica2.read(1, 1);
  ASSERT_TRUE(actions2.await(Seconds(2.0)));
  ASSERT_TRUE(actions2.isReady());
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

  os::rmdir(path);
}


TEST(CoordinatorTest, Elect)
{
  const std::string path1 = os::getcwd() + "/.log1";
  const std::string path2 = os::getcwd() + "/.log2";

  os::rmdir(path1);
  os::rmdir(path2);

  Replica replica1(path1);
  Replica replica2(path2);

  Network network;

  network.add(replica1.pid());
  network.add(replica2.pid());

  Coordinator coord(2, &replica1, &network);

  {
    Result<uint64_t> result = coord.elect(Timeout(Seconds(2.0)));
    ASSERT_TRUE(result.isSome());
    EXPECT_EQ(0u, result.get());
  }

  {
    Future<std::list<Action> > actions = replica1.read(0, 0);
    ASSERT_TRUE(actions.await(Seconds(2.0)));
    ASSERT_TRUE(actions.isReady());
    ASSERT_EQ(1u, actions.get().size());
    EXPECT_EQ(0u, actions.get().front().position());
    ASSERT_TRUE(actions.get().front().has_type());
    ASSERT_EQ(Action::NOP, actions.get().front().type());
  }

  os::rmdir(path1);
  os::rmdir(path2);
}


TEST(CoordinatorTest, AppendRead)
{
  const std::string path1 = os::getcwd() + "/.log1";
  const std::string path2 = os::getcwd() + "/.log2";

  os::rmdir(path1);
  os::rmdir(path2);

  Replica replica1(path1);
  Replica replica2(path2);

  Network network;

  network.add(replica1.pid());
  network.add(replica2.pid());

  Coordinator coord(2, &replica1, &network);

  {
    Result<uint64_t> result = coord.elect(Timeout(Seconds(2.0)));
    ASSERT_TRUE(result.isSome());
    EXPECT_EQ(0u, result.get());
  }

  uint64_t position;

  {
    Result<uint64_t> result2 =
      coord.append("hello world", Timeout(Seconds(2.0)));
    ASSERT_TRUE(result2.isSome());
    position = result2.get();
    EXPECT_EQ(1u, position);
  }

  {
    Future<std::list<Action> > actions = replica1.read(position, position);
    ASSERT_TRUE(actions.await(Seconds(2.0)));
    ASSERT_TRUE(actions.isReady());
    ASSERT_EQ(1u, actions.get().size());
    EXPECT_EQ(position, actions.get().front().position());
    ASSERT_TRUE(actions.get().front().has_type());
    ASSERT_EQ(Action::APPEND, actions.get().front().type());
    EXPECT_EQ("hello world", actions.get().front().append().bytes());
  }

  os::rmdir(path1);
  os::rmdir(path2);
}


TEST(CoordinatorTest, AppendReadError)
{
  const std::string path1 = os::getcwd() + "/.log1";
  const std::string path2 = os::getcwd() + "/.log2";

  os::rmdir(path1);
  os::rmdir(path2);

  Replica replica1(path1);
  Replica replica2(path2);

  Network network;

  network.add(replica1.pid());
  network.add(replica2.pid());

  Coordinator coord(2, &replica1, &network);

  {
    Result<uint64_t> result = coord.elect(Timeout(Seconds(2.0)));
    ASSERT_TRUE(result.isSome());
    EXPECT_EQ(0u, result.get());
  }

  uint64_t position;

  {
    Result<uint64_t> result2 =
      coord.append("hello world", Timeout(Seconds(2.0)));
    ASSERT_TRUE(result2.isSome());
    position = result2.get();
    EXPECT_EQ(1u, position);
  }

  {
    position += 1;
    Future<std::list<Action> > actions = replica1.read(position, position);
    ASSERT_TRUE(actions.await(Seconds(2.0)));
    ASSERT_TRUE(actions.isFailed());
    EXPECT_EQ("Bad read range (past end of log)", actions.failure());
  }

  os::rmdir(path1);
  os::rmdir(path2);
}


// TODO(benh): The coordinator tests that rely on timeouts can't rely
// on pausing the clock because when they get run with other tests
// there could be some lingering timeouts that cause the clock to
// advance such that the timeout within Coordinator::elect or
// Coordinator::append get started beyond what we expect. If this
// happens it doesn't matter what we "advance" the clock by, since
// certain orderings might still cause the test to hang, waiting for a
// future that started later than expected because the clock got
// updated unknowingly. This would be solved if Coordinator was
// actually a Process because then it would have a time associated
// with it and all processes that it creates (transitively). There
// might be a way to fix this by giving threads a similar role in
// libprocess, but until that happens these tests do not use the clock
// and are therefore disabled by default so as not to pause the tests
// for random unknown periods of time (but can still be run manually).

TEST(CoordinatorTest, DISABLED_ElectNoQuorum)
{
  const std::string path = os::getcwd() + "/.log";

  os::rmdir(path);

  Replica replica(path);

  Network network;

  network.add(replica.pid());

  Coordinator coord(2, &replica, &network);

  {
    Result<uint64_t> result = coord.elect(Timeout(Seconds(2.0)));
    ASSERT_TRUE(result.isNone());
  }

  os::rmdir(path);
}


TEST(CoordinatorTest, DISABLED_AppendNoQuorum)
{
  const std::string path1 = os::getcwd() + "/.log1";
  const std::string path2 = os::getcwd() + "/.log2";

  os::rmdir(path1);
  os::rmdir(path2);

  Replica replica1(path1);
  Replica replica2(path2);

  Network network;

  network.add(replica1.pid());
  network.add(replica2.pid());

  Coordinator coord(2, &replica1, &network);

  {
    Result<uint64_t> result = coord.elect(Timeout(Seconds(2.0)));
    ASSERT_TRUE(result.isSome());
    EXPECT_EQ(0u, result.get());
  }

  network.remove(replica1.pid());

  {
    Result<uint64_t> result =
      coord.append("hello world", Timeout(Seconds(2.0)));
    ASSERT_TRUE(result.isNone());
  }

  os::rmdir(path1);
  os::rmdir(path2);
}


TEST(CoordinatorTest, Failover)
{
  const std::string path1 = os::getcwd() + "/.log1";
  const std::string path2 = os::getcwd() + "/.log2";

  os::rmdir(path1);
  os::rmdir(path2);

  Replica replica1(path1);
  Replica replica2(path2);

  Network network1;

  network1.add(replica1.pid());
  network1.add(replica2.pid());

  Coordinator coord1(2, &replica1, &network1);

  {
    Result<uint64_t> result = coord1.elect(Timeout(Seconds(2.0)));
    ASSERT_TRUE(result.isSome());
    EXPECT_EQ(0u, result.get());
  }

  uint64_t position;

  {
    Result<uint64_t> result = coord1.append("hello world", Timeout(Seconds(2.0)));
    ASSERT_TRUE(result.isSome());
    position = result.get();
    EXPECT_EQ(1u, position);
  }

  Network network2;

  network2.add(replica1.pid());
  network2.add(replica2.pid());

  Coordinator coord2(2, &replica2, &network2);

  {
    Result<uint64_t> result = coord2.elect(Timeout(Seconds(2.0)));
    ASSERT_TRUE(result.isSome());
    EXPECT_EQ(position, result.get());
  }

  {
    Future<std::list<Action> > actions = replica2.read(position, position);
    ASSERT_TRUE(actions.await(Seconds(2.0)));
    ASSERT_TRUE(actions.isReady());
    ASSERT_EQ(1u, actions.get().size());
    EXPECT_EQ(position, actions.get().front().position());
    ASSERT_TRUE(actions.get().front().has_type());
    ASSERT_EQ(Action::APPEND, actions.get().front().type());
    EXPECT_EQ("hello world", actions.get().front().append().bytes());
  }

  os::rmdir(path1);
  os::rmdir(path2);
}


TEST(CoordinatorTest, Demoted)
{
  const std::string path1 = os::getcwd() + "/.log1";
  const std::string path2 = os::getcwd() + "/.log2";

  os::rmdir(path1);
  os::rmdir(path2);

  Replica replica1(path1);
  Replica replica2(path2);

  Network network1;

  network1.add(replica1.pid());
  network1.add(replica2.pid());

  Coordinator coord1(2, &replica1, &network1);

  {
    Result<uint64_t> result = coord1.elect(Timeout(Seconds(2.0)));
    ASSERT_TRUE(result.isSome());
    EXPECT_EQ(0u, result.get());
  }

  uint64_t position;

  {
    Result<uint64_t> result =
      coord1.append("hello world", Timeout(Seconds(2.0)));
    ASSERT_TRUE(result.isSome());
    position = result.get();
    EXPECT_EQ(1u, position);
  }

  Network network2;

  network2.add(replica1.pid());
  network2.add(replica2.pid());

  Coordinator coord2(2, &replica2, &network2);

  {
    Result<uint64_t> result = coord2.elect(Timeout(Seconds(2.0)));
    ASSERT_TRUE(result.isSome());
    EXPECT_EQ(position, result.get());
  }

  {
    Result<uint64_t> result =
      coord1.append("hello moto", Timeout(Seconds(2.0)));
    ASSERT_TRUE(result.isError());
    EXPECT_EQ("Coordinator demoted", result.error());
  }

  {
    Result<uint64_t> result =
      coord2.append("hello hello", Timeout(Seconds(2.0)));
    ASSERT_TRUE(result.isSome());
    position = result.get();
    EXPECT_EQ(2u, position);
  }

  {
    Future<std::list<Action> > actions = replica2.read(position, position);
    ASSERT_TRUE(actions.await(Seconds(2.0)));
    ASSERT_TRUE(actions.isReady());
    ASSERT_EQ(1u, actions.get().size());
    EXPECT_EQ(position, actions.get().front().position());
    ASSERT_TRUE(actions.get().front().has_type());
    ASSERT_EQ(Action::APPEND, actions.get().front().type());
    EXPECT_EQ("hello hello", actions.get().front().append().bytes());
  }

  os::rmdir(path1);
  os::rmdir(path2);
}


TEST(CoordinatorTest, Fill)
{
  const std::string path1 = os::getcwd() + "/.log1";
  const std::string path2 = os::getcwd() + "/.log2";
  const std::string path3 = os::getcwd() + "/.log3";

  os::rmdir(path1);
  os::rmdir(path2);
  os::rmdir(path3);

  Replica replica1(path1);
  Replica replica2(path2);

  Network network1;

  network1.add(replica1.pid());
  network1.add(replica2.pid());

  Coordinator coord1(2, &replica1, &network1);

  {
    Result<uint64_t> result = coord1.elect(Timeout(Seconds(2.0)));
    ASSERT_TRUE(result.isSome());
    EXPECT_EQ(0u, result.get());
  }

  uint64_t position;

  {
    Result<uint64_t> result =
      coord1.append("hello world", Timeout(Seconds(2.0)));
    ASSERT_TRUE(result.isSome());
    position = result.get();
    EXPECT_EQ(1u, position);
  }

  Replica replica3(path3);

  Network network2;

  network2.add(replica2.pid());
  network2.add(replica3.pid());

  Coordinator coord2(2, &replica3, &network2);

  {
    Result<uint64_t> result = coord2.elect(Timeout(Seconds(2.0)));
    ASSERT_TRUE(result.isNone());
    result = coord2.elect(Timeout(Seconds(2.0)));
    ASSERT_TRUE(result.isSome());
    EXPECT_EQ(position, result.get());
  }

  {
    Future<std::list<Action> > actions = replica3.read(position, position);
    ASSERT_TRUE(actions.await(Seconds(2.0)));
    ASSERT_TRUE(actions.isReady());
    ASSERT_EQ(1u, actions.get().size());
    EXPECT_EQ(position, actions.get().front().position());
    ASSERT_TRUE(actions.get().front().has_type());
    ASSERT_EQ(Action::APPEND, actions.get().front().type());
    EXPECT_EQ("hello world", actions.get().front().append().bytes());
  }

  os::rmdir(path1);
  os::rmdir(path2);
  os::rmdir(path3);
}


TEST(CoordinatorTest, NotLearnedFill)
{
  MockFilter filter;
  process::filter(&filter);

  EXPECT_MESSAGE(filter, _, _, _)
    .WillRepeatedly(Return(false));

  EXPECT_MESSAGE(filter, Eq(LearnedMessage().GetTypeName()), _, _)
    .WillRepeatedly(Return(true));

  const std::string path1 = os::getcwd() + "/.log1";
  const std::string path2 = os::getcwd() + "/.log2";
  const std::string path3 = os::getcwd() + "/.log3";

  os::rmdir(path1);
  os::rmdir(path2);
  os::rmdir(path3);

  Replica replica1(path1);
  Replica replica2(path2);

  Network network1;

  network1.add(replica1.pid());
  network1.add(replica2.pid());

  Coordinator coord1(2, &replica1, &network1);

  {
    Result<uint64_t> result = coord1.elect(Timeout(Seconds(2.0)));
    ASSERT_TRUE(result.isSome());
    EXPECT_EQ(0u, result.get());
  }

  uint64_t position;

  {
    Result<uint64_t> result =
      coord1.append("hello world", Timeout(Seconds(2.0)));
    ASSERT_TRUE(result.isSome());
    position = result.get();
    EXPECT_EQ(1u, position);
  }

  Replica replica3(path3);

  Network network2;

  network2.add(replica2.pid());
  network2.add(replica3.pid());

  Coordinator coord2(2, &replica3, &network2);

  {
    Result<uint64_t> result = coord2.elect(Timeout(Seconds(2.0)));
    ASSERT_TRUE(result.isNone());
    result = coord2.elect(Timeout(Seconds(2.0)));
    ASSERT_TRUE(result.isSome());
    EXPECT_EQ(position, result.get());
  }

  {
    Future<std::list<Action> > actions = replica3.read(position, position);
    ASSERT_TRUE(actions.await(Seconds(2.0)));
    ASSERT_TRUE(actions.isReady());
    ASSERT_EQ(1u, actions.get().size());
    EXPECT_EQ(position, actions.get().front().position());
    ASSERT_TRUE(actions.get().front().has_type());
    ASSERT_EQ(Action::APPEND, actions.get().front().type());
    EXPECT_EQ("hello world", actions.get().front().append().bytes());
  }

  os::rmdir(path1);
  os::rmdir(path2);
  os::rmdir(path3);

  process::filter(NULL);
}


TEST(CoordinatorTest, MultipleAppends)
{
  const std::string path1 = os::getcwd() + "/.log1";
  const std::string path2 = os::getcwd() + "/.log2";

  os::rmdir(path1);
  os::rmdir(path2);

  Replica replica1(path1);
  Replica replica2(path2);

  Network network;

  network.add(replica1.pid());
  network.add(replica2.pid());

  Coordinator coord(2, &replica1, &network);

  {
    Result<uint64_t> result = coord.elect(Timeout(Seconds(2.0)));
    ASSERT_TRUE(result.isSome());
    EXPECT_EQ(0u, result.get());
  }

  for (uint64_t position = 1; position <= 10; position++) {
    Result<uint64_t> result =
      coord.append(stringify(position), Timeout(Seconds(2.0)));
    ASSERT_TRUE(result.isSome());
    EXPECT_EQ(position, result.get());
  }

  {
    Future<std::list<Action> > actions = replica1.read(1, 10);
    ASSERT_TRUE(actions.await(Seconds(2.0)));
    ASSERT_TRUE(actions.isReady());
    EXPECT_EQ(10u, actions.get().size());
    foreach (const Action& action, actions.get()) {
      ASSERT_TRUE(action.has_type());
      ASSERT_EQ(Action::APPEND, action.type());
      EXPECT_EQ(stringify(action.position()), action.append().bytes());
    }
  }

  os::rmdir(path1);
  os::rmdir(path2);
}


TEST(CoordinatorTest, MultipleAppendsNotLearnedFill)
{
  MockFilter filter;
  process::filter(&filter);

  EXPECT_MESSAGE(filter, _, _, _)
    .WillRepeatedly(Return(false));

  EXPECT_MESSAGE(filter, Eq(LearnedMessage().GetTypeName()), _, _)
    .WillRepeatedly(Return(true));

  const std::string path1 = os::getcwd() + "/.log1";
  const std::string path2 = os::getcwd() + "/.log2";
  const std::string path3 = os::getcwd() + "/.log3";

  os::rmdir(path1);
  os::rmdir(path2);
  os::rmdir(path3);

  Replica replica1(path1);
  Replica replica2(path2);

  Network network1;

  network1.add(replica1.pid());
  network1.add(replica2.pid());

  Coordinator coord1(2, &replica1, &network1);

  {
    Result<uint64_t> result = coord1.elect(Timeout(Seconds(2.0)));
    ASSERT_TRUE(result.isSome());
    EXPECT_EQ(0u, result.get());
  }

  for (uint64_t position = 1; position <= 10; position++) {
    Result<uint64_t> result =
      coord1.append(stringify(position), Timeout(Seconds(2.0)));
    ASSERT_TRUE(result.isSome());
    EXPECT_EQ(position, result.get());
  }

  Replica replica3(path3);

  Network network2;

  network2.add(replica2.pid());
  network2.add(replica3.pid());

  Coordinator coord2(2, &replica3, &network2);

  {
    Result<uint64_t> result = coord2.elect(Timeout(Seconds(2.0)));
    ASSERT_TRUE(result.isNone());
    result = coord2.elect(Timeout(Seconds(2.0)));
    ASSERT_TRUE(result.isSome());
    EXPECT_EQ(10u, result.get());
  }

  {
    Future<std::list<Action> > actions = replica3.read(1, 10);
    ASSERT_TRUE(actions.await(Seconds(2.0)));
    ASSERT_TRUE(actions.isReady());
    EXPECT_EQ(10u, actions.get().size());
    foreach (const Action& action, actions.get()) {
      ASSERT_TRUE(action.has_type());
      ASSERT_EQ(Action::APPEND, action.type());
      EXPECT_EQ(stringify(action.position()), action.append().bytes());
    }
  }

  os::rmdir(path1);
  os::rmdir(path2);
  os::rmdir(path3);

  process::filter(NULL);
}


TEST(CoordinatorTest, Truncate)
{
  const std::string path1 = os::getcwd() + "/.log1";
  const std::string path2 = os::getcwd() + "/.log2";

  os::rmdir(path1);
  os::rmdir(path2);

  Replica replica1(path1);
  Replica replica2(path2);

  Network network;

  network.add(replica1.pid());
  network.add(replica2.pid());

  Coordinator coord(2, &replica1, &network);

  {
    Result<uint64_t> result = coord.elect(Timeout(Seconds(2.0)));
    ASSERT_TRUE(result.isSome());
    EXPECT_EQ(0u, result.get());
  }

  for (uint64_t position = 1; position <= 10; position++) {
    Result<uint64_t> result =
      coord.append(stringify(position), Timeout(Seconds(2.0)));
    ASSERT_TRUE(result.isSome());
    EXPECT_EQ(position, result.get());
  }

  {
    Result<uint64_t> result = coord.truncate(7, Timeout(Seconds(2.0)));
    ASSERT_TRUE(result.isSome());
    EXPECT_EQ(11u, result.get());
  }

  {
    Future<std::list<Action> > actions = replica1.read(6, 10);
    ASSERT_TRUE(actions.await(Seconds(2.0)));
    ASSERT_TRUE(actions.isFailed());
    EXPECT_EQ("Bad read range (truncated position)", actions.failure());
  }

  {
    Future<std::list<Action> > actions = replica1.read(7, 10);
    ASSERT_TRUE(actions.await(Seconds(2.0)));
    ASSERT_TRUE(actions.isReady());
    EXPECT_EQ(4u, actions.get().size());
    foreach (const Action& action, actions.get()) {
      ASSERT_TRUE(action.has_type());
      ASSERT_EQ(Action::APPEND, action.type());
      EXPECT_EQ(stringify(action.position()), action.append().bytes());
    }
  }

  os::rmdir(path1);
  os::rmdir(path2);
}


TEST(CoordinatorTest, TruncateNotLearnedFill)
{
  MockFilter filter;
  process::filter(&filter);

  EXPECT_MESSAGE(filter, _, _, _)
    .WillRepeatedly(Return(false));

  EXPECT_MESSAGE(filter, Eq(LearnedMessage().GetTypeName()), _, _)
    .WillRepeatedly(Return(true));

  const std::string path1 = os::getcwd() + "/.log1";
  const std::string path2 = os::getcwd() + "/.log2";
  const std::string path3 = os::getcwd() + "/.log3";

  os::rmdir(path1);
  os::rmdir(path2);
  os::rmdir(path3);

  Replica replica1(path1);
  Replica replica2(path2);

  Network network1;

  network1.add(replica1.pid());
  network1.add(replica2.pid());

  Coordinator coord1(2, &replica1, &network1);

  {
    Result<uint64_t> result = coord1.elect(Timeout(Seconds(2.0)));
    ASSERT_TRUE(result.isSome());
    EXPECT_EQ(0u, result.get());
  }

  for (uint64_t position = 1; position <= 10; position++) {
    Result<uint64_t> result =
      coord1.append(stringify(position), Timeout(Seconds(2.0)));
    ASSERT_TRUE(result.isSome());
    EXPECT_EQ(position, result.get());
  }

  {
    Result<uint64_t> result = coord1.truncate(7, Timeout(Seconds(2.0)));
    ASSERT_TRUE(result.isSome());
    EXPECT_EQ(11u, result.get());
  }

  Replica replica3(path3);

  Network network2;

  network2.add(replica2.pid());
  network2.add(replica3.pid());

  Coordinator coord2(2, &replica3, &network2);

  {
    Result<uint64_t> result = coord2.elect(Timeout(Seconds(2.0)));
    ASSERT_TRUE(result.isNone());
    result = coord2.elect(Timeout(Seconds(2.0)));
    ASSERT_TRUE(result.isSome());
    EXPECT_EQ(11u, result.get());
  }

  {
    Future<std::list<Action> > actions = replica3.read(6, 10);
    ASSERT_TRUE(actions.await(Seconds(2.0)));
    ASSERT_TRUE(actions.isFailed());
    EXPECT_EQ("Bad read range (truncated position)", actions.failure());
  }

  {
    Future<std::list<Action> > actions = replica3.read(7, 10);
    ASSERT_TRUE(actions.await(Seconds(2.0)));
    ASSERT_TRUE(actions.isReady());
    EXPECT_EQ(4u, actions.get().size());
    foreach (const Action& action, actions.get()) {
      ASSERT_TRUE(action.has_type());
      ASSERT_EQ(Action::APPEND, action.type());
      EXPECT_EQ(stringify(action.position()), action.append().bytes());
    }
  }

  os::rmdir(path1);
  os::rmdir(path2);
  os::rmdir(path3);

  process::filter(NULL);
}


TEST(CoordinatorTest, TruncateLearnedFill)
{
  const std::string path1 = os::getcwd() + "/.log1";
  const std::string path2 = os::getcwd() + "/.log2";
  const std::string path3 = os::getcwd() + "/.log3";

  os::rmdir(path1);
  os::rmdir(path2);
  os::rmdir(path3);

  Replica replica1(path1);
  Replica replica2(path2);

  Network network1;

  network1.add(replica1.pid());
  network1.add(replica2.pid());

  Coordinator coord1(2, &replica1, &network1);

  {
    Result<uint64_t> result = coord1.elect(Timeout(Seconds(2.0)));
    ASSERT_TRUE(result.isSome());
    EXPECT_EQ(0u, result.get());
  }

  for (uint64_t position = 1; position <= 10; position++) {
    Result<uint64_t> result =
      coord1.append(stringify(position), Timeout(Seconds(2.0)));
    ASSERT_TRUE(result.isSome());
    EXPECT_EQ(position, result.get());
  }

  {
    Result<uint64_t> result = coord1.truncate(7, Timeout(Seconds(2.0)));
    ASSERT_TRUE(result.isSome());
    EXPECT_EQ(11u, result.get());
  }

  Replica replica3(path3);

  Network network2;

  network2.add(replica2.pid());
  network2.add(replica3.pid());

  Coordinator coord2(2, &replica3, &network2);

  {
    Result<uint64_t> result = coord2.elect(Timeout(Seconds(2.0)));
    ASSERT_TRUE(result.isNone());
    result = coord2.elect(Timeout(Seconds(2.0)));
    ASSERT_TRUE(result.isSome());
    EXPECT_EQ(11u, result.get());
  }

  {
    Future<std::list<Action> > actions = replica3.read(6, 10);
    ASSERT_TRUE(actions.await(Seconds(2.0)));
    ASSERT_TRUE(actions.isFailed());
    EXPECT_EQ("Bad read range (truncated position)", actions.failure());
  }

  {
    Future<std::list<Action> > actions = replica3.read(7, 10);
    ASSERT_TRUE(actions.await(Seconds(2.0)));
    ASSERT_TRUE(actions.isReady());
    EXPECT_EQ(4u, actions.get().size());
    foreach (const Action& action, actions.get()) {
      ASSERT_TRUE(action.has_type());
      ASSERT_EQ(Action::APPEND, action.type());
      EXPECT_EQ(stringify(action.position()), action.append().bytes());
    }
  }

  os::rmdir(path1);
  os::rmdir(path2);
  os::rmdir(path3);
}


TEST(LogTest, WriteRead)
{
  const std::string path1 = os::getcwd() + "/.log1";
  const std::string path2 = os::getcwd() + "/.log2";

  os::rmdir(path1);
  os::rmdir(path2);

  Replica replica1(path1);

  std::set<UPID> pids;
  pids.insert(replica1.pid());

  Log log(2, path2, pids);

  Log::Writer writer(&log, Timeout(Seconds(2.0)));

  Result<Log::Position> position =
    writer.append("hello world", Timeout(Seconds(2.0)));

  ASSERT_TRUE(position.isSome());

  Log::Reader reader(&log);

  Result<std::list<Log::Entry> > entries =
    reader.read(position.get(), position.get(), Timeout(Seconds(2.0)));

  ASSERT_TRUE(entries.isSome());
  ASSERT_EQ(1u, entries.get().size());
  EXPECT_EQ(position.get(), entries.get().front().position);
  EXPECT_EQ("hello world", entries.get().front().data);

  os::rmdir(path1);
  os::rmdir(path2);
}


TEST(LogTest, Position)
{
  const std::string path1 = os::getcwd() + "/.log1";
  const std::string path2 = os::getcwd() + "/.log2";

  os::rmdir(path1);
  os::rmdir(path2);

  Replica replica1(path1);

  std::set<UPID> pids;
  pids.insert(replica1.pid());

  Log log(2, path2, pids);

  Log::Writer writer(&log, Timeout(Seconds(2.0)));

  Result<Log::Position> position =
    writer.append("hello world", Timeout(Seconds(2.0)));

  ASSERT_TRUE(position.isSome());

  ASSERT_EQ(position.get(), log.position(position.get().identity()));

  os::rmdir(path1);
  os::rmdir(path2);
}


TEST(CoordinatorTest, RacingElect) {}

TEST(CoordinatorTest, FillNoQuorum) {}

TEST(CoordinatorTest, FillInconsistent) {}

TEST(CoordinatorTest, LearnedOnOneReplica_NotLearnedOnAnother) {}

TEST(CoordinatorTest, LearnedOnOneReplica_NotLearnedOnAnother_AnotherFailsAndRecovers) {}
