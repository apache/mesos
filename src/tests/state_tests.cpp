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
#include <process/gtest.hpp>
#include <process/protobuf.hpp>

#include <stout/gtest.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/try.hpp>

#include "common/type_utils.hpp"

#include "master/registry.hpp"

#include "state/leveldb.hpp"
#include "state/protobuf.hpp"
#include "state/storage.hpp"
#include "state/zookeeper.hpp"

#ifdef MESOS_HAS_JAVA
#include "tests/zookeeper.hpp"
#endif

using namespace mesos;
using namespace mesos::internal;

using namespace process;

using mesos::internal::registry::Registry;
using mesos::internal::registry::Slave;

using state::LevelDBStorage;
using state::Storage;
#ifdef MESOS_HAS_JAVA
using state::ZooKeeperStorage;
#endif

using state::protobuf::State;
using state::protobuf::Variable;


void FetchAndStoreAndFetch(State* state)
{
  Future<Variable<Registry> > future1 = state->fetch<Registry>("registry");
  AWAIT_READY(future1);

  Variable<Registry> variable = future1.get();

  Registry registry1 = variable.get();
  EXPECT_TRUE(registry1.slaves().size() == 0);

  Slave* slave = registry1.add_slaves();
  slave->mutable_info()->set_hostname("localhost");

  variable = variable.mutate(registry1);

  Future<Option<Variable<Registry> > > future2 = state->store(variable);
  AWAIT_READY(future2);
  ASSERT_SOME(future2.get());

  future1 = state->fetch<Registry>("registry");
  AWAIT_READY(future1);

  variable = future1.get();

  Registry registry2 = variable.get();
  ASSERT_TRUE(registry2.slaves().size() == 1);
  EXPECT_EQ("localhost", registry2.slaves(0).info().hostname());
}


void FetchAndStoreAndStoreAndFetch(State* state)
{
  Future<Variable<Registry> > future1 = state->fetch<Registry>("registry");
  AWAIT_READY(future1);

  Variable<Registry> variable = future1.get();

  Registry registry1 = variable.get();
  EXPECT_TRUE(registry1.slaves().size() == 0);

  Slave* slave = registry1.add_slaves();
  slave->mutable_info()->set_hostname("localhost");

  variable = variable.mutate(registry1);

  Future<Option<Variable<Registry> > > future2 = state->store(variable);
  AWAIT_READY(future2);
  ASSERT_SOME(future2.get());

  variable = future2.get().get();

  future2 = state->store(variable);
  AWAIT_READY(future2);
  ASSERT_SOME(future2.get());

  future1 = state->fetch<Registry>("registry");
  AWAIT_READY(future1);

  variable = future1.get();

  Registry registry2 = variable.get();
  ASSERT_TRUE(registry2.slaves().size() == 1);
  EXPECT_EQ("localhost", registry2.slaves(0).info().hostname());
}


void FetchAndStoreAndStoreFailAndFetch(State* state)
{
  Future<Variable<Registry> > future1 = state->fetch<Registry>("registry");
  AWAIT_READY(future1);

  Variable<Registry> variable1 = future1.get();

  Registry registry1 = variable1.get();
  EXPECT_TRUE(registry1.slaves().size() == 0);

  Slave* slave1 = registry1.add_slaves();
  slave1->mutable_info()->set_hostname("localhost1");

  Variable<Registry> variable2 = variable1.mutate(registry1);

  Future<Option<Variable<Registry> > > future2 = state->store(variable2);
  AWAIT_READY(future2);
  ASSERT_SOME(future2.get());

  Registry registry2 = variable1.get();
  EXPECT_TRUE(registry2.slaves().size() == 0);

  Slave* slave2 = registry2.add_slaves();
  slave2->mutable_info()->set_hostname("localhost2");

  variable2 = variable1.mutate(registry2);

  future2 = state->store(variable2);
  AWAIT_READY(future2);
  EXPECT_TRUE(future2.get().isNone());

  future1 = state->fetch<Registry>("registry");
  AWAIT_READY(future1);

  variable1 = future1.get();

  registry1 = variable1.get();
  ASSERT_TRUE(registry1.slaves().size() == 1);
  EXPECT_EQ("localhost1", registry1.slaves(0).info().hostname());
}


void FetchAndStoreAndExpungeAndFetch(State* state)
{
  Future<Variable<Registry> > future1 = state->fetch<Registry>("registry");
  AWAIT_READY(future1);

  Variable<Registry> variable = future1.get();

  Registry registry1 = variable.get();
  EXPECT_TRUE(registry1.slaves().size() == 0);

  Slave* slave = registry1.add_slaves();
  slave->mutable_info()->set_hostname("localhost");

  variable = variable.mutate(registry1);

  Future<Option<Variable<Registry> > > future2 = state->store(variable);
  AWAIT_READY(future2);
  ASSERT_SOME(future2.get());

  variable = future2.get().get();

  Future<bool> future3 = state->expunge(variable);
  AWAIT_READY(future3);
  ASSERT_TRUE(future3.get());

  future1 = state->fetch<Registry>("registry");
  AWAIT_READY(future1);

  variable = future1.get();

  Registry registry2 = variable.get();
  ASSERT_EQ(0, registry2.slaves().size());
}


void FetchAndStoreAndExpungeAndExpunge(State* state)
{
  Future<Variable<Registry> > future1 = state->fetch<Registry>("registry");
  AWAIT_READY(future1);

  Variable<Registry> variable = future1.get();

  Registry registry1 = variable.get();
  EXPECT_TRUE(registry1.slaves().size() == 0);

  Slave* slave = registry1.add_slaves();
  slave->mutable_info()->set_hostname("localhost");

  variable = variable.mutate(registry1);

  Future<Option<Variable<Registry> > > future2 = state->store(variable);
  AWAIT_READY(future2);
  ASSERT_SOME(future2.get());

  variable = future2.get().get();

  Future<bool> future3 = state->expunge(variable);
  AWAIT_READY(future3);
  ASSERT_TRUE(future3.get());

  future3 = state->expunge(variable);
  AWAIT_READY(future3);
  ASSERT_FALSE(future3.get());
}


void FetchAndStoreAndExpungeAndStoreAndFetch(State* state)
{
  Future<Variable<Registry> > future1 = state->fetch<Registry>("registry");
  AWAIT_READY(future1);

  Variable<Registry> variable = future1.get();

  Registry registry1 = variable.get();
  EXPECT_TRUE(registry1.slaves().size() == 0);

  Slave* slave = registry1.add_slaves();
  slave->mutable_info()->set_hostname("localhost");

  variable = variable.mutate(registry1);

  Future<Option<Variable<Registry> > > future2 = state->store(variable);
  AWAIT_READY(future2);
  ASSERT_SOME(future2.get());

  variable = future2.get().get();

  Future<bool> future3 = state->expunge(variable);
  AWAIT_READY(future3);
  ASSERT_TRUE(future3.get());

  future2 = state->store(variable);
  AWAIT_READY(future2);
  ASSERT_SOME(future2.get());

  future1 = state->fetch<Registry>("registry");
  AWAIT_READY(future1);

  variable = future1.get();

  Registry registry2 = variable.get();
  ASSERT_TRUE(registry2.slaves().size() == 1);
  EXPECT_EQ("localhost", registry2.slaves(0).info().hostname());
}


void Names(State* state)
{
  Future<Variable<Registry> > future1 = state->fetch<Registry>("registry");
  AWAIT_READY(future1);

  Variable<Registry> variable = future1.get();

  Registry registry1 = variable.get();
  EXPECT_TRUE(registry1.slaves().size() == 0);

  Slave* slave = registry1.add_slaves();
  slave->mutable_info()->set_hostname("localhost");

  variable = variable.mutate(registry1);

  Future<Option<Variable<Registry> > > future2 = state->store(variable);
  AWAIT_READY(future2);
  ASSERT_SOME(future2.get());

  Future<std::vector<std::string> > names = state->names();
  AWAIT_READY(names);
  ASSERT_TRUE(names.get().size() == 1);
  EXPECT_EQ("registry", names.get()[0]);
}


class LevelDBStateTest : public ::testing::Test
{
public:
  LevelDBStateTest()
    : storage(NULL),
      state(NULL),
      path(os::getcwd() + "/.state") {}

protected:
  virtual void SetUp()
  {
    os::rmdir(path);
    storage = new state::LevelDBStorage(path);
    state = new State(storage);
  }

  virtual void TearDown()
  {
    delete state;
    delete storage;
    os::rmdir(path);
  }

  state::Storage* storage;
  State* state;

private:
  const std::string path;
};


TEST_F(LevelDBStateTest, FetchAndStoreAndFetch)
{
  FetchAndStoreAndFetch(state);
}


TEST_F(LevelDBStateTest, FetchAndStoreAndStoreAndFetch)
{
  FetchAndStoreAndStoreAndFetch(state);
}


TEST_F(LevelDBStateTest, FetchAndStoreAndStoreFailAndFetch)
{
  FetchAndStoreAndStoreFailAndFetch(state);
}


TEST_F(LevelDBStateTest, FetchAndStoreAndExpungeAndFetch)
{
  FetchAndStoreAndExpungeAndFetch(state);
}


TEST_F(LevelDBStateTest, FetchAndStoreAndExpungeAndExpunge)
{
  FetchAndStoreAndExpungeAndExpunge(state);
}


TEST_F(LevelDBStateTest, FetchAndStoreAndExpungeAndStoreAndFetch)
{
  FetchAndStoreAndExpungeAndStoreAndFetch(state);
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
    : storage(NULL),
      state(NULL) {}

protected:
  virtual void SetUp()
  {
    ZooKeeperTest::SetUp();
    storage = new state::ZooKeeperStorage(
        server->connectString(),
        NO_TIMEOUT,
        "/state/");
    state = new State(storage);
  }

  virtual void TearDown()
  {
    delete state;
    delete storage;
    ZooKeeperTest::TearDown();
  }

  state::Storage* storage;
  State* state;
};


TEST_F(ZooKeeperStateTest, FetchAndStoreAndFetch)
{
  FetchAndStoreAndFetch(state);
}


TEST_F(ZooKeeperStateTest, FetchAndStoreAndStoreAndFetch)
{
  FetchAndStoreAndStoreAndFetch(state);
}


TEST_F(ZooKeeperStateTest, FetchAndStoreAndStoreFailAndFetch)
{
  FetchAndStoreAndStoreFailAndFetch(state);
}


TEST_F(ZooKeeperStateTest, FetchAndStoreAndExpungeAndFetch)
{
  FetchAndStoreAndExpungeAndFetch(state);
}


TEST_F(ZooKeeperStateTest, FetchAndStoreAndExpungeAndExpunge)
{
  FetchAndStoreAndExpungeAndExpunge(state);
}


TEST_F(ZooKeeperStateTest, FetchAndStoreAndExpungeAndStoreAndFetch)
{
  FetchAndStoreAndExpungeAndStoreAndFetch(state);
}


TEST_F(ZooKeeperStateTest, Names)
{
  Names(state);
}
#endif // MESOS_HAS_JAVA
