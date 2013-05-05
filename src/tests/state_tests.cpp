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
#include <vector>

#include <mesos/mesos.hpp>

#include <process/future.hpp>
#include <process/protobuf.hpp>

#include <stout/option.hpp>
#include <stout/os.hpp>

#include "common/type_utils.hpp"

#include "messages/messages.hpp"

#include "state/leveldb.hpp"
#include "state/serializer.hpp"
#include "state/state.hpp"
#include "state/zookeeper.hpp"

#include "tests/utils.hpp"
#ifdef MESOS_HAS_JAVA
#include "tests/zookeeper.hpp"
#endif

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::state;

using namespace process;


void GetSetGet(State<ProtobufSerializer>* state)
{
  Future<Variable<Slaves> > variable = state->get<Slaves>("slaves");
  AWAIT_READY(variable);

  Variable<Slaves> slaves1 = variable.get();
  EXPECT_TRUE(slaves1->infos().size() == 0);

  SlaveInfo info;
  info.set_hostname("localhost");
  info.set_webui_hostname("localhost");

  slaves1->add_infos()->MergeFrom(info);

  Future<Option<Variable<Slaves> > > result = state->set(slaves1);

  result.await();

  ASSERT_TRUE(result.isReady());
  ASSERT_SOME(result.get());

  variable = state->get<Slaves>("slaves");

  variable.await();

  ASSERT_TRUE(variable.isReady());

  Variable<Slaves> slaves2 = variable.get();

  ASSERT_TRUE(slaves2->infos().size() == 1);
  EXPECT_EQ("localhost", slaves2->infos(0).hostname());
  EXPECT_EQ("localhost", slaves2->infos(0).webui_hostname());
}


void GetSetSetGet(State<ProtobufSerializer>* state)
{
  Future<Variable<Slaves> > variable = state->get<Slaves>("slaves");
  AWAIT_READY(variable);

  Variable<Slaves> slaves1 = variable.get();
  EXPECT_TRUE(slaves1->infos().size() == 0);

  SlaveInfo info;
  info.set_hostname("localhost");
  info.set_webui_hostname("localhost");

  slaves1->add_infos()->MergeFrom(info);

  Future<Option<Variable<Slaves> > > result = state->set(slaves1);

  result.await();

  ASSERT_TRUE(result.isReady());
  ASSERT_SOME(result.get());

  slaves1 = result.get().get();

  result = state->set(slaves1);

  result.await();

  ASSERT_TRUE(result.isReady());
  ASSERT_SOME(result.get());

  variable = state->get<Slaves>("slaves");

  variable.await();

  ASSERT_TRUE(variable.isReady());

  Variable<Slaves> slaves2 = variable.get();

  ASSERT_TRUE(slaves2->infos().size() == 1);
  EXPECT_EQ("localhost", slaves2->infos(0).hostname());
  EXPECT_EQ("localhost", slaves2->infos(0).webui_hostname());
}


void GetGetSetSetGet(State<ProtobufSerializer>* state)
{
  Future<Variable<Slaves> > variable = state->get<Slaves>("slaves");
  AWAIT_READY(variable);

  Variable<Slaves> slaves1 = variable.get();
  EXPECT_TRUE(slaves1->infos().size() == 0);

  variable = state->get<Slaves>("slaves");
  AWAIT_READY(variable);

  Variable<Slaves> slaves2 = variable.get();
  EXPECT_TRUE(slaves2->infos().size() == 0);

  SlaveInfo info2;
  info2.set_hostname("localhost2");
  info2.set_webui_hostname("localhost2");

  slaves2->add_infos()->MergeFrom(info2);

  Future<Option<Variable<Slaves> > > result = state->set(slaves2);

  result.await();

  ASSERT_TRUE(result.isReady());
  ASSERT_SOME(result.get());

  SlaveInfo info1;
  info1.set_hostname("localhost1");
  info1.set_webui_hostname("localhost1");

  slaves1->add_infos()->MergeFrom(info1);

  result = state->set(slaves1);
  AWAIT_READY(result);
  EXPECT_TRUE(result.get().isNone());

  variable = state->get<Slaves>("slaves");
  AWAIT_READY(variable);

  slaves1 = variable.get();

  ASSERT_TRUE(slaves1->infos().size() == 1);
  EXPECT_EQ("localhost2", slaves1->infos(0).hostname());
  EXPECT_EQ("localhost2", slaves1->infos(0).webui_hostname());
}


void Names(State<ProtobufSerializer>* state)
{
  Future<Variable<Slaves> > variable = state->get<Slaves>("slaves");
  AWAIT_READY(variable);

  Variable<Slaves> slaves1 = variable.get();
  EXPECT_TRUE(slaves1->infos().size() == 0);

  SlaveInfo info;
  info.set_hostname("localhost");
  info.set_webui_hostname("localhost");

  slaves1->add_infos()->MergeFrom(info);

  Future<Option<Variable<Slaves> > > result = state->set(slaves1);

  result.await();

  ASSERT_TRUE(result.isReady());
  EXPECT_SOME(result.get());

  Future<std::vector<std::string> > names = state->names();

  names.await();

  ASSERT_TRUE(names.isReady());
  ASSERT_TRUE(names.get().size() == 1);
  EXPECT_EQ("slaves", names.get()[0]);
}


class LevelDBStateTest : public ::testing::Test
{
public:
  LevelDBStateTest()
    : state(NULL), path(os::getcwd() + "/.state") {}

protected:
  virtual void SetUp()
  {
    os::rmdir(path);
    state = new LevelDBState<ProtobufSerializer>(path);
  }

  virtual void TearDown()
  {
    delete state;
    os::rmdir(path);
  }

  State<ProtobufSerializer>* state;

private:
  const std::string path;
};


TEST_F(LevelDBStateTest, GetSetGet)
{
  GetSetGet(state);
}


TEST_F(LevelDBStateTest, GetSetSetGet)
{
  GetSetSetGet(state);
}


TEST_F(LevelDBStateTest, GetGetSetSetGet)
{
  GetGetSetSetGet(state);
}


TEST_F(LevelDBStateTest, Names)
{
  Names(state);
}


#ifdef MESOS_HAS_JAVA
class ZooKeeperStateTest : public tests::ZooKeeperTest
{
public:
  ZooKeeperStateTest()
    : state(NULL) {}

protected:
  virtual void SetUp()
  {
    ZooKeeperTest::SetUp();
    state = new ZooKeeperState<ProtobufSerializer>(
        server->connectString(),
        NO_TIMEOUT,
        "/state/");
  }

  virtual void TearDown()
  {
    delete state;
    ZooKeeperTest::TearDown();
  }

  State<ProtobufSerializer>* state;
};


TEST_F(ZooKeeperStateTest, GetSetGet)
{
  GetSetGet(state);
}


TEST_F(ZooKeeperStateTest, GetSetSetGet)
{
  GetSetSetGet(state);
}


TEST_F(ZooKeeperStateTest, GetGetSetSetGet)
{
  GetGetSetSetGet(state);
}

TEST_F(ZooKeeperStateTest, Names)
{
  Names(state);
}
#endif // MESOS_HAS_JAVA
