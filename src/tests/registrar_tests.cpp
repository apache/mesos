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

#include <map>
#include <string>

#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/pid.hpp>
#include <process/process.hpp>

#include "common/protobuf_utils.hpp"
#include "common/type_utils.hpp"

#include "master/flags.hpp"
#include "master/master.hpp"
#include "master/registrar.hpp"

#include "state/leveldb.hpp"
#include "state/protobuf.hpp"
#include "state/storage.hpp"

using namespace mesos;
using namespace mesos::internal;

using namespace process;

using std::map;
using std::string;

using testing::_;
using testing::Eq;

namespace mesos {
namespace internal {
namespace master {

class RegistrarTest : public ::testing::TestWithParam<bool>
{
public:
  RegistrarTest()
    : storage(NULL),
      state(NULL),
      path(os::getcwd() + "/.state") {}

protected:
  virtual void SetUp()
  {
    os::rmdir(path);
    // TODO(bmahler): Only use LevelDBStorage or LogStorage for
    // performance testing, otherwise just use InMemoryStorage.
    storage = new state::LevelDBStorage(path);
    state = new state::protobuf::State(storage);
    master.CopyFrom(protobuf::createMasterInfo(UPID("master@127.0.0.1:5050")));
    flags.registry_strict = GetParam();
  }

  virtual void TearDown()
  {
    delete state;
    delete storage;
    os::rmdir(path);
  }

  state::Storage* storage;
  state::protobuf::State* state;
  MasterInfo master;
  Flags flags;

private:
  const std::string path;
};


// The Registrar tests are parameterized by "strictness".
INSTANTIATE_TEST_CASE_P(Strict, RegistrarTest, ::testing::Bool());


TEST_P(RegistrarTest, recover)
{
  Registrar registrar(flags, state);

  SlaveInfo slave;
  slave.set_hostname("localhost");
  SlaveID id;
  id.set_value("1");
  slave.mutable_id()->CopyFrom(id);

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


TEST_P(RegistrarTest, admit)
{
  Registrar registrar(flags, state);
  AWAIT_READY(registrar.recover(master));

  SlaveInfo info1;
  info1.set_hostname("localhost");

  SlaveID id1;
  id1.set_value("1");
  info1.mutable_id()->CopyFrom(id1);

  AWAIT_EQ(true, registrar.apply(Owned<Operation>(new AdmitSlave(info1))));

  if (flags.registry_strict) {
    AWAIT_EQ(false, registrar.apply(Owned<Operation>(new AdmitSlave(info1))));
  } else {
    AWAIT_EQ(true, registrar.apply(Owned<Operation>(new AdmitSlave(info1))));
  }
}


TEST_P(RegistrarTest, readmit)
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


TEST_P(RegistrarTest, remove)
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


TEST_P(RegistrarTest, bootstrap)
{
  SlaveID id;
  id.set_value("1");

  SlaveInfo info;
  info.set_hostname("localhost");
  info.mutable_id()->CopyFrom(id);

  // Run 1 readmits a slave that is not present.
  {
    Registrar registrar(flags, state);
    AWAIT_READY(registrar.recover(master));

    // If not strict, we should be allowed to readmit the slave.
    if (flags.registry_strict) {
      AWAIT_EQ(false,
               registrar.apply(Owned<Operation>(new ReadmitSlave(info))));
    } else {
      AWAIT_EQ(true,
               registrar.apply(Owned<Operation>(new ReadmitSlave(info))));
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
      EXPECT_EQ(info, registry.get().slaves().slaves(0).info());
    }
  }
}

} // namespace master {
} // namespace internal {
} // namespace mesos {
