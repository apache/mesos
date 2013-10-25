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

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/pid.hpp>
#include <process/protobuf.hpp>
#include <process/timeout.hpp>

#include <stout/gtest.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>

#include "common/type_utils.hpp"

#include "log/coordinator.hpp"
#include "log/log.hpp"
#include "log/replica.hpp"

#include "messages/messages.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::log;

using process::Clock;
using process::Future;
using process::Timeout;
using process::UPID;

using testing::_;
using testing::Eq;
using testing::Return;

#include "tests/utils.hpp"

using namespace mesos::internal::tests;

class ReplicaTest : public TemporaryDirectoryTest {};


TEST_F(ReplicaTest, Promise)
{
  const std::string path = os::getcwd() + "/.log";

  Replica replica(path);

  PromiseRequest request;
  PromiseResponse response;
  Future<PromiseResponse> future;

  request.set_id(2);

  future = protocol::promise(replica.pid(), request);

  AWAIT_READY(future);

  response = future.get();
  EXPECT_TRUE(response.okay());
  EXPECT_EQ(2u, response.id());
  EXPECT_TRUE(response.has_position());
  EXPECT_EQ(0u, response.position());
  EXPECT_FALSE(response.has_action());

  request.set_id(1);

  future = protocol::promise(replica.pid(), request);

  AWAIT_READY(future);

  response = future.get();
  EXPECT_FALSE(response.okay());
  EXPECT_EQ(1u, response.id());
  EXPECT_FALSE(response.has_position());
  EXPECT_FALSE(response.has_action());

  request.set_id(3);

  future = protocol::promise(replica.pid(), request);

  AWAIT_READY(future);

  response = future.get();
  EXPECT_TRUE(response.okay());
  EXPECT_EQ(3u, response.id());
  EXPECT_TRUE(response.has_position());
  EXPECT_EQ(0u, response.position());
  EXPECT_FALSE(response.has_action());
}


TEST_F(ReplicaTest, Append)
{
  const std::string path = os::getcwd() + "/.log";

  Replica replica(path);

  const uint64_t id = 1;

  PromiseRequest request1;
  request1.set_id(id);

  Future<PromiseResponse> future1 =
    protocol::promise(replica.pid(), request1);

  AWAIT_READY(future1);

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

  AWAIT_READY(future2);

  WriteResponse response2 = future2.get();
  EXPECT_TRUE(response2.okay());
  EXPECT_EQ(id, response2.id());
  EXPECT_EQ(1u, response2.position());

  Future<std::list<Action> > actions = replica.read(1, 1);

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


TEST_F(ReplicaTest, Recover)
{
  const std::string path = os::getcwd() + "/.log";

  Replica replica1(path);

  const uint64_t id = 1;

  PromiseRequest request1;
  request1.set_id(id);

  Future<PromiseResponse> future1 =
    protocol::promise(replica1.pid(), request1);

  AWAIT_READY(future1);

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

  AWAIT_READY(future2);

  WriteResponse response2 = future2.get();
  EXPECT_TRUE(response2.okay());
  EXPECT_EQ(id, response2.id());
  EXPECT_EQ(1u, response2.position());

  Future<std::list<Action> > actions1 = replica1.read(1, 1);

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

  Future<std::list<Action> > actions2 = replica2.read(1, 1);

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


class CoordinatorTest : public TemporaryDirectoryTest {};


TEST_F(CoordinatorTest, Elect)
{
  const std::string path1 = os::getcwd() + "/.log1";
  const std::string path2 = os::getcwd() + "/.log2";

  Replica replica1(path1);
  Replica replica2(path2);

  Network network;

  network.add(replica1.pid());
  network.add(replica2.pid());

  Coordinator coord(2, &replica1, &network);

  {
    Result<uint64_t> result = coord.elect(Timeout::in(Seconds(10)));
    ASSERT_SOME(result);
    EXPECT_EQ(0u, result.get());
  }

  {
    Future<std::list<Action> > actions = replica1.read(0, 0);
    AWAIT_READY(actions);
    ASSERT_EQ(1u, actions.get().size());
    EXPECT_EQ(0u, actions.get().front().position());
    ASSERT_TRUE(actions.get().front().has_type());
    ASSERT_EQ(Action::NOP, actions.get().front().type());
  }
}


TEST_F(CoordinatorTest, AppendRead)
{
  const std::string path1 = os::getcwd() + "/.log1";
  const std::string path2 = os::getcwd() + "/.log2";

  Replica replica1(path1);
  Replica replica2(path2);

  Network network;

  network.add(replica1.pid());
  network.add(replica2.pid());

  Coordinator coord(2, &replica1, &network);

  {
    Result<uint64_t> result = coord.elect(Timeout::in(Seconds(10)));
    ASSERT_SOME(result);
    EXPECT_EQ(0u, result.get());
  }

  uint64_t position;

  {
    Result<uint64_t> result2 =
      coord.append("hello world", Timeout::in(Seconds(10)));
    ASSERT_SOME(result2);
    position = result2.get();
    EXPECT_EQ(1u, position);
  }

  {
    Future<std::list<Action> > actions = replica1.read(position, position);
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
  const std::string path1 = os::getcwd() + "/.log1";
  const std::string path2 = os::getcwd() + "/.log2";

  Replica replica1(path1);
  Replica replica2(path2);

  Network network;

  network.add(replica1.pid());
  network.add(replica2.pid());

  Coordinator coord(2, &replica1, &network);

  {
    Result<uint64_t> result = coord.elect(Timeout::in(Seconds(10)));
    ASSERT_SOME(result);
    EXPECT_EQ(0u, result.get());
  }

  uint64_t position;

  {
    Result<uint64_t> result2 =
      coord.append("hello world", Timeout::in(Seconds(10)));
    ASSERT_SOME(result2);
    position = result2.get();
    EXPECT_EQ(1u, position);
  }

  {
    position += 1;
    Future<std::list<Action> > actions = replica1.read(position, position);
    AWAIT_FAILED(actions);
    EXPECT_EQ("Bad read range (past end of log)", actions.failure());
  }
}


TEST_F(CoordinatorTest, ElectNoQuorum)
{
  const std::string path = os::getcwd() + "/.log";

  Replica replica(path);

  Network network;

  network.add(replica.pid());

  Coordinator coord(2, &replica, &network);

  Clock::pause();

  // Create a timeout here so that we can advance time.
  Timeout timeout = Timeout::in(Seconds(10));

  Clock::advance(Seconds(10));

  {
    Result<uint64_t> result = coord.elect(timeout);
    EXPECT_TRUE(result.isNone());
  }

  Clock::resume();
}


TEST_F(CoordinatorTest, AppendNoQuorum)
{
  const std::string path1 = os::getcwd() + "/.log1";
  const std::string path2 = os::getcwd() + "/.log2";

  Replica replica1(path1);
  Replica replica2(path2);

  Network network;

  network.add(replica1.pid());
  network.add(replica2.pid());

  Coordinator coord(2, &replica1, &network);

  {
    Result<uint64_t> result = coord.elect(Timeout::in(Seconds(10)));
    ASSERT_SOME(result);
    EXPECT_EQ(0u, result.get());
  }

  network.remove(replica2.pid());

  Clock::pause();

  // Create a timeout here so that we can advance time.
  Timeout timeout = Timeout::in(Seconds(10));

  Clock::advance(Seconds(10));

  {
    Result<uint64_t> result = coord.append("hello world", timeout);
    EXPECT_TRUE(result.isNone());
  }

  Clock::resume();
}


TEST_F(CoordinatorTest, Failover)
{
  const std::string path1 = os::getcwd() + "/.log1";
  const std::string path2 = os::getcwd() + "/.log2";

  Replica replica1(path1);
  Replica replica2(path2);

  Network network1;

  network1.add(replica1.pid());
  network1.add(replica2.pid());

  Coordinator coord1(2, &replica1, &network1);

  {
    Result<uint64_t> result = coord1.elect(Timeout::in(Seconds(10)));
    ASSERT_SOME(result);
    EXPECT_EQ(0u, result.get());
  }

  uint64_t position;

  {
    Result<uint64_t> result =
      coord1.append("hello world", Timeout::in(Seconds(10)));
    ASSERT_SOME(result);
    position = result.get();
    EXPECT_EQ(1u, position);
  }

  Network network2;

  network2.add(replica1.pid());
  network2.add(replica2.pid());

  Coordinator coord2(2, &replica2, &network2);

  {
    Result<uint64_t> result = coord2.elect(Timeout::in(Seconds(10)));
    ASSERT_SOME(result);
    EXPECT_EQ(position, result.get());
  }

  {
    Future<std::list<Action> > actions = replica2.read(position, position);
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
  const std::string path1 = os::getcwd() + "/.log1";
  const std::string path2 = os::getcwd() + "/.log2";

  Replica replica1(path1);
  Replica replica2(path2);

  Network network1;

  network1.add(replica1.pid());
  network1.add(replica2.pid());

  Coordinator coord1(2, &replica1, &network1);

  {
    Result<uint64_t> result = coord1.elect(Timeout::in(Seconds(10)));
    ASSERT_SOME(result);
    EXPECT_EQ(0u, result.get());
  }

  uint64_t position;

  {
    Result<uint64_t> result =
      coord1.append("hello world", Timeout::in(Seconds(10)));
    ASSERT_SOME(result);
    position = result.get();
    EXPECT_EQ(1u, position);
  }

  Network network2;

  network2.add(replica1.pid());
  network2.add(replica2.pid());

  Coordinator coord2(2, &replica2, &network2);

  {
    Result<uint64_t> result = coord2.elect(Timeout::in(Seconds(10)));
    ASSERT_SOME(result);
    EXPECT_EQ(position, result.get());
  }

  {
    Result<uint64_t> result =
      coord1.append("hello moto", Timeout::in(Seconds(10)));
    ASSERT_TRUE(result.isError());
    EXPECT_EQ("Coordinator demoted", result.error());
  }

  {
    Result<uint64_t> result =
      coord2.append("hello hello", Timeout::in(Seconds(10)));
    ASSERT_SOME(result);
    position = result.get();
    EXPECT_EQ(2u, position);
  }

  {
    Future<std::list<Action> > actions = replica2.read(position, position);
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
  const std::string path1 = os::getcwd() + "/.log1";
  const std::string path2 = os::getcwd() + "/.log2";
  const std::string path3 = os::getcwd() + "/.log3";

  Replica replica1(path1);
  Replica replica2(path2);

  Network network1;

  network1.add(replica1.pid());
  network1.add(replica2.pid());

  Coordinator coord1(2, &replica1, &network1);

  {
    Result<uint64_t> result = coord1.elect(Timeout::in(Seconds(10)));
    ASSERT_SOME(result);
    EXPECT_EQ(0u, result.get());
  }

  uint64_t position;

  {
    Result<uint64_t> result =
      coord1.append("hello world", Timeout::in(Seconds(10)));
    ASSERT_SOME(result);
    position = result.get();
    EXPECT_EQ(1u, position);
  }

  Replica replica3(path3);

  Network network2;

  network2.add(replica2.pid());
  network2.add(replica3.pid());

  Coordinator coord2(2, &replica3, &network2);

  {
    Result<uint64_t> result = coord2.elect(Timeout::in(Seconds(10)));
    ASSERT_TRUE(result.isNone());
    result = coord2.elect(Timeout::in(Seconds(10)));
    ASSERT_SOME(result);
    EXPECT_EQ(position, result.get());
  }

  {
    Future<std::list<Action> > actions = replica3.read(position, position);
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
  DROP_MESSAGES(Eq(LearnedMessage().GetTypeName()), _, _);

  const std::string path1 = os::getcwd() + "/.log1";
  const std::string path2 = os::getcwd() + "/.log2";
  const std::string path3 = os::getcwd() + "/.log3";

  Replica replica1(path1);
  Replica replica2(path2);

  Network network1;

  network1.add(replica1.pid());
  network1.add(replica2.pid());

  Coordinator coord1(2, &replica1, &network1);

  {
    Result<uint64_t> result = coord1.elect(Timeout::in(Seconds(10)));
    ASSERT_SOME(result);
    EXPECT_EQ(0u, result.get());
  }

  uint64_t position;

  {
    Result<uint64_t> result =
      coord1.append("hello world", Timeout::in(Seconds(10)));
    ASSERT_SOME(result);
    position = result.get();
    EXPECT_EQ(1u, position);
  }

  Replica replica3(path3);

  Network network2;

  network2.add(replica2.pid());
  network2.add(replica3.pid());

  Coordinator coord2(2, &replica3, &network2);

  {
    Result<uint64_t> result = coord2.elect(Timeout::in(Seconds(10)));
    ASSERT_TRUE(result.isNone());
    result = coord2.elect(Timeout::in(Seconds(10)));
    ASSERT_SOME(result);
    EXPECT_EQ(position, result.get());
  }

  {
    Future<std::list<Action> > actions = replica3.read(position, position);
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
  const std::string path1 = os::getcwd() + "/.log1";
  const std::string path2 = os::getcwd() + "/.log2";

  Replica replica1(path1);
  Replica replica2(path2);

  Network network;

  network.add(replica1.pid());
  network.add(replica2.pid());

  Coordinator coord(2, &replica1, &network);

  {
    Result<uint64_t> result = coord.elect(Timeout::in(Seconds(10)));
    ASSERT_SOME(result);
    EXPECT_EQ(0u, result.get());
  }

  for (uint64_t position = 1; position <= 10; position++) {
    Result<uint64_t> result =
      coord.append(stringify(position), Timeout::in(Seconds(10)));
    ASSERT_SOME(result);
    EXPECT_EQ(position, result.get());
  }

  {
    Future<std::list<Action> > actions = replica1.read(1, 10);
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
  DROP_MESSAGES(Eq(LearnedMessage().GetTypeName()), _, _);

  const std::string path1 = os::getcwd() + "/.log1";
  const std::string path2 = os::getcwd() + "/.log2";
  const std::string path3 = os::getcwd() + "/.log3";

  Replica replica1(path1);
  Replica replica2(path2);

  Network network1;

  network1.add(replica1.pid());
  network1.add(replica2.pid());

  Coordinator coord1(2, &replica1, &network1);

  {
    Result<uint64_t> result = coord1.elect(Timeout::in(Seconds(10)));
    ASSERT_SOME(result);
    EXPECT_EQ(0u, result.get());
  }

  for (uint64_t position = 1; position <= 10; position++) {
    Result<uint64_t> result =
      coord1.append(stringify(position), Timeout::in(Seconds(10)));
    ASSERT_SOME(result);
    EXPECT_EQ(position, result.get());
  }

  Replica replica3(path3);

  Network network2;

  network2.add(replica2.pid());
  network2.add(replica3.pid());

  Coordinator coord2(2, &replica3, &network2);

  {
    Result<uint64_t> result = coord2.elect(Timeout::in(Seconds(10)));
    ASSERT_TRUE(result.isNone());
    result = coord2.elect(Timeout::in(Seconds(10)));
    ASSERT_SOME(result);
    EXPECT_EQ(10u, result.get());
  }

  {
    Future<std::list<Action> > actions = replica3.read(1, 10);
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
  const std::string path1 = os::getcwd() + "/.log1";
  const std::string path2 = os::getcwd() + "/.log2";

  Replica replica1(path1);
  Replica replica2(path2);

  Network network;

  network.add(replica1.pid());
  network.add(replica2.pid());

  Coordinator coord(2, &replica1, &network);

  {
    Result<uint64_t> result = coord.elect(Timeout::in(Seconds(10)));
    ASSERT_SOME(result);
    EXPECT_EQ(0u, result.get());
  }

  for (uint64_t position = 1; position <= 10; position++) {
    Result<uint64_t> result =
      coord.append(stringify(position), Timeout::in(Seconds(10)));
    ASSERT_SOME(result);
    EXPECT_EQ(position, result.get());
  }

  {
    Result<uint64_t> result = coord.truncate(7, Timeout::in(Seconds(10)));
    ASSERT_SOME(result);
    EXPECT_EQ(11u, result.get());
  }

  {
    Future<std::list<Action> > actions = replica1.read(6, 10);
    AWAIT_FAILED(actions);
    EXPECT_EQ("Bad read range (truncated position)", actions.failure());
  }

  {
    Future<std::list<Action> > actions = replica1.read(7, 10);
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
  DROP_MESSAGES(Eq(LearnedMessage().GetTypeName()), _, _);

  const std::string path1 = os::getcwd() + "/.log1";
  const std::string path2 = os::getcwd() + "/.log2";
  const std::string path3 = os::getcwd() + "/.log3";

  Replica replica1(path1);
  Replica replica2(path2);

  Network network1;

  network1.add(replica1.pid());
  network1.add(replica2.pid());

  Coordinator coord1(2, &replica1, &network1);

  {
    Result<uint64_t> result = coord1.elect(Timeout::in(Seconds(10)));
    ASSERT_SOME(result);
    EXPECT_EQ(0u, result.get());
  }

  for (uint64_t position = 1; position <= 10; position++) {
    Result<uint64_t> result =
      coord1.append(stringify(position), Timeout::in(Seconds(10)));
    ASSERT_SOME(result);
    EXPECT_EQ(position, result.get());
  }

  {
    Result<uint64_t> result = coord1.truncate(7, Timeout::in(Seconds(10)));
    ASSERT_SOME(result);
    EXPECT_EQ(11u, result.get());
  }

  Replica replica3(path3);

  Network network2;

  network2.add(replica2.pid());
  network2.add(replica3.pid());

  Coordinator coord2(2, &replica3, &network2);

  {
    Result<uint64_t> result = coord2.elect(Timeout::in(Seconds(10)));
    ASSERT_TRUE(result.isNone());
    result = coord2.elect(Timeout::in(Seconds(10)));
    ASSERT_SOME(result);
    EXPECT_EQ(11u, result.get());
  }

  {
    Future<std::list<Action> > actions = replica3.read(6, 10);
    AWAIT_FAILED(actions);
    EXPECT_EQ("Bad read range (truncated position)", actions.failure());
  }

  {
    Future<std::list<Action> > actions = replica3.read(7, 10);
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
  const std::string path1 = os::getcwd() + "/.log1";
  const std::string path2 = os::getcwd() + "/.log2";
  const std::string path3 = os::getcwd() + "/.log3";

  Replica replica1(path1);
  Replica replica2(path2);

  Network network1;

  network1.add(replica1.pid());
  network1.add(replica2.pid());

  Coordinator coord1(2, &replica1, &network1);

  {
    Result<uint64_t> result = coord1.elect(Timeout::in(Seconds(10)));
    ASSERT_SOME(result);
    EXPECT_EQ(0u, result.get());
  }

  for (uint64_t position = 1; position <= 10; position++) {
    Result<uint64_t> result =
      coord1.append(stringify(position), Timeout::in(Seconds(10)));
    ASSERT_SOME(result);
    EXPECT_EQ(position, result.get());
  }

  {
    Result<uint64_t> result = coord1.truncate(7, Timeout::in(Seconds(10)));
    ASSERT_SOME(result);
    EXPECT_EQ(11u, result.get());
  }

  Replica replica3(path3);

  Network network2;

  network2.add(replica2.pid());
  network2.add(replica3.pid());

  Coordinator coord2(2, &replica3, &network2);

  {
    Result<uint64_t> result = coord2.elect(Timeout::in(Seconds(10)));
    ASSERT_TRUE(result.isNone());
    result = coord2.elect(Timeout::in(Seconds(10)));
    ASSERT_SOME(result);
    EXPECT_EQ(11u, result.get());
  }

  {
    Future<std::list<Action> > actions = replica3.read(6, 10);
    AWAIT_FAILED(actions);
    EXPECT_EQ("Bad read range (truncated position)", actions.failure());
  }

  {
    Future<std::list<Action> > actions = replica3.read(7, 10);
    AWAIT_READY(actions);
    EXPECT_EQ(4u, actions.get().size());
    foreach (const Action& action, actions.get()) {
      ASSERT_TRUE(action.has_type());
      ASSERT_EQ(Action::APPEND, action.type());
      EXPECT_EQ(stringify(action.position()), action.append().bytes());
    }
  }
}


class LogTest : public TemporaryDirectoryTest {};


TEST_F(LogTest, WriteRead)
{
  const std::string path1 = os::getcwd() + "/.log1";
  const std::string path2 = os::getcwd() + "/.log2";

  Replica replica1(path1);

  std::set<UPID> pids;
  pids.insert(replica1.pid());

  Log log(2, path2, pids);

  Log::Writer writer(&log, Seconds(10));

  Result<Log::Position> position =
    writer.append("hello world", Timeout::in(Seconds(10)));

  ASSERT_SOME(position);

  Log::Reader reader(&log);

  Result<std::list<Log::Entry> > entries =
    reader.read(position.get(), position.get(), Timeout::in(Seconds(10)));

  ASSERT_SOME(entries);
  ASSERT_EQ(1u, entries.get().size());
  EXPECT_EQ(position.get(), entries.get().front().position);
  EXPECT_EQ("hello world", entries.get().front().data);
}


TEST_F(LogTest, Position)
{
  const std::string path1 = os::getcwd() + "/.log1";
  const std::string path2 = os::getcwd() + "/.log2";

  Replica replica1(path1);

  std::set<UPID> pids;
  pids.insert(replica1.pid());

  Log log(2, path2, pids);

  Log::Writer writer(&log, Seconds(10));

  Result<Log::Position> position =
    writer.append("hello world", Timeout::in(Seconds(10)));

  ASSERT_SOME(position);

  ASSERT_EQ(position.get(), log.position(position.get().identity()));
}


TEST_F(CoordinatorTest, RacingElect) {}

TEST_F(CoordinatorTest, FillNoQuorum) {}

TEST_F(CoordinatorTest, FillInconsistent) {}

TEST_F(CoordinatorTest, LearnedOnOneReplica_NotLearnedOnAnother) {}

TEST_F(CoordinatorTest,
       LearnedOnOneReplica_NotLearnedOnAnother_AnotherFailsAndRecovers) {}
