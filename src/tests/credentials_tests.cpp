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

#include <stout/uri.hpp>

#include <process/gmock.hpp>
#include <process/owned.hpp>
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

using mesos::master::detector::MasterDetector;

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
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);
  ASSERT_NE("", slaveRegisteredMessage->slave_id().value());
}

// Using JSON base file for authentication without
// protobuf tools assistance.
TEST_F(CredentialsTest, AuthenticatedSlaveJSON)
{
  string path = path::join(os::getcwd(), "credentials");

  Try<int_fd> fd = os::open(
      path,
      O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
      S_IRUSR | S_IWUSR | S_IRGRP);

  ASSERT_SOME(fd);

  // This unit tests our capacity to process JSON credentials without
  // using our protobuf tools.
  JSON::Object credential;
  credential.values["principal"] = DEFAULT_CREDENTIAL.principal();
  credential.values["secret"] = DEFAULT_CREDENTIAL.secret();

  JSON::Array array;
  array.values.push_back(credential);

  JSON::Object credentials;
  credentials.values["credentials"] = array;

  ASSERT_SOME(os::write(fd.get(), stringify(credentials)))
      << "Failed to write credentials to '" << path << "'";

  ASSERT_SOME(os::close(fd.get()));

  map<string, Option<string>> values{
    {"credentials", Some(uri::from_path(path))}};

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.load(values, true);

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.load(values, true);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);
  ASSERT_NE("", slaveRegisteredMessage->slave_id().value());
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
