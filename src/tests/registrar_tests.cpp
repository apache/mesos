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

class RegistrarTest : public ::testing::Test
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
    storage = new state::LevelDBStorage(path);
    state = new state::protobuf::State(storage);
  }

  virtual void TearDown()
  {
    delete state;
    delete storage;
    os::rmdir(path);
  }

  state::Storage* storage;
  state::protobuf::State* state;

private:
  const std::string path;
};


TEST_F(RegistrarTest, admit)
{
  Registrar registrar(state);

  SlaveInfo info1;
  info1.set_hostname("localhost");

  // Missing ID results in a Failure.
  AWAIT_EXPECT_FAILED(registrar.admit(info1));

  SlaveID id1;
  id1.set_value("1");
  info1.mutable_id()->CopyFrom(id1);

  AWAIT_EQ(true, registrar.admit(info1));
  AWAIT_EQ(false, registrar.admit(info1));
}


TEST_F(RegistrarTest, readmit)
{
  Registrar registrar(state);

  SlaveInfo info1;
  info1.set_hostname("localhost");

  // Missing ID results in a failure.
  AWAIT_EXPECT_FAILED(registrar.readmit(info1));

  SlaveID id1;
  id1.set_value("1");
  info1.mutable_id()->CopyFrom(id1);

  SlaveID id2;
  id2.set_value("2");

  SlaveInfo info2;
  info2.set_hostname("localhost");
  info2.mutable_id()->CopyFrom(id2);

  AWAIT_EQ(true, registrar.admit(info1));

  AWAIT_EQ(true, registrar.readmit(info1));

  AWAIT_EQ(false, registrar.readmit(info2));
}


TEST_F(RegistrarTest, remove)
{
  Registrar registrar(state);

  SlaveInfo info1;
  info1.set_hostname("localhost");

  // Missing ID results in a Failure.
  AWAIT_EXPECT_FAILED(registrar.remove(info1));

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

  AWAIT_EQ(true, registrar.admit(info1));
  AWAIT_EQ(true, registrar.admit(info2));
  AWAIT_EQ(true, registrar.admit(info3));

  AWAIT_EQ(true, registrar.remove(info1));
  AWAIT_EQ(false, registrar.remove(info1));

  AWAIT_EQ(true, registrar.admit(info1));

  AWAIT_EQ(true, registrar.remove(info2));
  AWAIT_EQ(false, registrar.remove(info2));

  AWAIT_EQ(true, registrar.remove(info3));
  AWAIT_EQ(false, registrar.remove(info3));
}

} // namespace master {
} // namespace internal {
} // namespace mesos {
