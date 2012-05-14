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

#include <mesos/mesos.hpp>

#include <process/future.hpp>
#include <process/protobuf.hpp>

#include "common/option.hpp"
#include "common/type_utils.hpp"
#include "common/utils.hpp"

#include "messages/messages.hpp"

#include "state/state.hpp"

#include "tests/base_zookeeper_test.hpp"
#include "tests/utils.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::state;
using namespace mesos::internal::test;

using namespace process;


void GetSetGet(State* state)
{
  Future<State::Variable<Slaves> > variable = state->get<Slaves>("slaves");

  variable.await();

  ASSERT_TRUE(variable.isReady());

  State::Variable<Slaves> slaves1 = variable.get();

  EXPECT_TRUE(slaves1->infos().size() == 0);

  SlaveInfo info;
  info.set_hostname("localhost");
  info.set_webui_hostname("localhost");

  slaves1->add_infos()->MergeFrom(info);

  Future<bool> result = state->set(&slaves1);

  result.await();

  ASSERT_TRUE(result.isReady());
  EXPECT_TRUE(result.get());

  variable = state->get<Slaves>("slaves");

  variable.await();

  ASSERT_TRUE(variable.isReady());

  State::Variable<Slaves> slaves2 = variable.get();

  ASSERT_TRUE(slaves2->infos().size() == 1);
  EXPECT_EQ("localhost", slaves2->infos(0).hostname());
  EXPECT_EQ("localhost", slaves2->infos(0).webui_hostname());
}


void GetSetSetGet(State* state)
{
  Future<State::Variable<Slaves> > variable = state->get<Slaves>("slaves");

  variable.await();

  ASSERT_TRUE(variable.isReady());

  State::Variable<Slaves> slaves1 = variable.get();

  EXPECT_TRUE(slaves1->infos().size() == 0);

  SlaveInfo info;
  info.set_hostname("localhost");
  info.set_webui_hostname("localhost");

  slaves1->add_infos()->MergeFrom(info);

  Future<bool> result = state->set(&slaves1);

  result.await();

  ASSERT_TRUE(result.isReady());
  EXPECT_TRUE(result.get());

  result = state->set(&slaves1);

  result.await();

  ASSERT_TRUE(result.isReady());
  EXPECT_TRUE(result.get());

  variable = state->get<Slaves>("slaves");

  variable.await();

  ASSERT_TRUE(variable.isReady());

  State::Variable<Slaves> slaves2 = variable.get();

  ASSERT_TRUE(slaves2->infos().size() == 1);
  EXPECT_EQ("localhost", slaves2->infos(0).hostname());
  EXPECT_EQ("localhost", slaves2->infos(0).webui_hostname());
}


void GetGetSetSetGet(State* state)
{
  Future<State::Variable<Slaves> > variable = state->get<Slaves>("slaves");

  variable.await();

  ASSERT_TRUE(variable.isReady());

  State::Variable<Slaves> slaves1 = variable.get();

  EXPECT_TRUE(slaves1->infos().size() == 0);

  variable = state->get<Slaves>("slaves");

  variable.await();

  ASSERT_TRUE(variable.isReady());

  State::Variable<Slaves> slaves2 = variable.get();

  EXPECT_TRUE(slaves2->infos().size() == 0);

  SlaveInfo info2;
  info2.set_hostname("localhost2");
  info2.set_webui_hostname("localhost2");

  slaves2->add_infos()->MergeFrom(info2);

  Future<bool> result = state->set(&slaves2);

  result.await();

  ASSERT_TRUE(result.isReady());
  EXPECT_TRUE(result.get());

  SlaveInfo info1;
  info1.set_hostname("localhost1");
  info1.set_webui_hostname("localhost1");

  slaves1->add_infos()->MergeFrom(info1);

  result = state->set(&slaves1);

  result.await();

  ASSERT_TRUE(result.isReady());
  EXPECT_FALSE(result.get());

  variable = state->get<Slaves>("slaves");

  variable.await();

  ASSERT_TRUE(variable.isReady());

  slaves1 = variable.get();

  ASSERT_TRUE(slaves1->infos().size() == 1);
  EXPECT_EQ("localhost2", slaves1->infos(0).hostname());
  EXPECT_EQ("localhost2", slaves1->infos(0).webui_hostname());
}


class LevelDBStateTest : public ::testing::Test
{
public:
  LevelDBStateTest()
    : state(NULL), path(utils::os::getcwd() + "/.state") {}

protected:
  virtual void SetUp()
  {
    utils::os::rmdir(path);
    state = new LevelDBState(path);
  }

  virtual void TearDown()
  {
    delete state;
    utils::os::rmdir(path);
  }

  State* state;

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


class ZooKeeperStateTest : public mesos::internal::test::BaseZooKeeperTest
{
public:
  ZooKeeperStateTest()
    : state(NULL) {}

protected:
  virtual void SetUp()
  {
    BaseZooKeeperTest::SetUp();
    state = new ZooKeeperState(zks->connectString(), NO_TIMEOUT, "/state/");
  }

  virtual void TearDown()
  {
    delete state;
    BaseZooKeeperTest::TearDown();
  }

  State* state;
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
