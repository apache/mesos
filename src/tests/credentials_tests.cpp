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

#include <string>

#include <gmock/gmock.h>

#include <process/gmock.hpp>
#include <process/pid.hpp>

#include "master/flags.hpp"
#include "master/master.hpp"
#include "tests/mesos.hpp"
#include "tests/utils.hpp"

using namespace process;

using std::map;
using std::string;
using std::vector;

using mesos::internal::master::Master;
using mesos::internal::slave::Slave;

using process::PID;

using testing::_;

namespace mesos {
namespace internal {
namespace tests {

class CredentialsTest : public MesosTest {};


// This test verifies that an authenticated slave is
// granted registration by the master.
TEST_F(CredentialsTest, AuthenticatedSlave)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Try<PID<Slave>> slave = StartSlave();
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);
  ASSERT_NE("", slaveRegisteredMessage.get().slave_id().value());

  Shutdown();
}


// Test verifing well executed credential authentication
// using text formatted credentials so as to test
// backwards compatibility.
TEST_F(CredentialsTest, AuthenticatedSlaveText)
{
  string path =  path::join(os::getcwd(), "credentials");

  Try<int> fd = os::open(
      path,
      O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
      S_IRUSR | S_IWUSR | S_IRGRP);

  CHECK_SOME(fd);

  string credentials =
    DEFAULT_CREDENTIAL.principal() + " " + DEFAULT_CREDENTIAL.secret();

  CHECK_SOME(os::write(fd.get(), credentials))
      << "Failed to write credentials to '" << path << "'";

  CHECK_SOME(os::close(fd.get()));

  map<string, Option<string>> values{{"credentials", Some("file://" + path)}};

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.load(values, true);

  Try<PID<Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.load(values, true);

  Try<PID<Slave>> slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);
  ASSERT_NE("", slaveRegisteredMessage.get().slave_id().value());

  Shutdown();
}


// Using JSON base file for authentication without
// protobuf tools assistance.
TEST_F(CredentialsTest, AuthenticatedSlaveJSON)
{
  string path =  path::join(os::getcwd(), "credentials");

  Try<int> fd = os::open(
      path,
      O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
      S_IRUSR | S_IWUSR | S_IRGRP);

  CHECK_SOME(fd);

  // This unit tests our capacity to process JSON credentials without
  // using our protobuf tools.
  JSON::Object credential;
  credential.values["principal"] = DEFAULT_CREDENTIAL.principal();
  credential.values["secret"] = DEFAULT_CREDENTIAL.secret();

  JSON::Array array;
  array.values.push_back(credential);

  JSON::Object credentials;
  credentials.values["credentials"] = array;

  CHECK_SOME(os::write(fd.get(), stringify(credentials)))
      << "Failed to write credentials to '" << path << "'";

  CHECK_SOME(os::close(fd.get()));

  map<string, Option<string>> values{{"credentials", Some("file://" + path)}};

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.load(values, true);

  Try<PID<Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.load(values, true);

  Try<PID<Slave>> slave = StartSlave(slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);
  ASSERT_NE("", slaveRegisteredMessage.get().slave_id().value());

  Shutdown();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
