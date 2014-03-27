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
#include <map>
#include <string>
#include <vector>

#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/pid.hpp>
#include <process/process.hpp>

#include <stout/bytes.hpp>
#include <stout/stopwatch.hpp>

#include "common/protobuf_utils.hpp"
#include "common/type_utils.hpp"

#include "master/flags.hpp"
#include "master/master.hpp"
#include "master/registrar.hpp"

#include "state/in_memory.hpp"
#include "state/leveldb.hpp"
#include "state/protobuf.hpp"
#include "state/storage.hpp"

using namespace mesos;
using namespace mesos::internal;

using namespace process;

using std::map;
using std::string;
using std::vector;

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
      state(NULL) {}

protected:
  virtual void SetUp()
  {
    // We use InMemoryStorage to test Registrar correctness and
    // LevelDBStorage to test performance.
    // TODO(xujyan): Use LogStorage to exercise what we're using in
    // production.
    storage = new state::InMemoryStorage();
    state = new state::protobuf::State(storage);
    master.CopyFrom(protobuf::createMasterInfo(UPID("master@127.0.0.1:5050")));
    flags.registry_strict = GetParam();
  }

  virtual void TearDown()
  {
    delete state;
    delete storage;
  }

  state::Storage* storage;
  state::protobuf::State* state;
  MasterInfo master;
  Flags flags;
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


// We are not inheriting from RegistrarTest because this test fixture
// derives from a different instantiation of the TestWithParam template.
class Registrar_BENCHMARK_Test : public ::testing::TestWithParam<size_t>
{
public:
  Registrar_BENCHMARK_Test()
    : storage(NULL),
      state(NULL),
      path(os::getcwd() + "/.state") {}

protected:
  virtual void SetUp()
  {
    os::rmdir(path);

    // We use InMemoryStorage to test Registrar correctness and
    // LevelDBStorage to test performance.
    storage = new state::LevelDBStorage(path);
    state = new state::protobuf::State(storage);
    master.CopyFrom(protobuf::createMasterInfo(UPID("master@127.0.0.1:5050")));

    // Strictness is not important in our benchmark tests so it's
    // just set to false here.
    flags.registry_strict = false;
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


// The Registrar benchmark tests are parameterized by the number of slaves.
INSTANTIATE_TEST_CASE_P(
    SlaveCount,
    Registrar_BENCHMARK_Test,
    ::testing::Values(10000U, 20000U, 30000U, 50000U));


TEST_P(Registrar_BENCHMARK_Test, performance)
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
  LOG(INFO) << "Removed " << slaveCount << " slaves in " << watch.elapsed();
}

} // namespace master {
} // namespace internal {
} // namespace mesos {
