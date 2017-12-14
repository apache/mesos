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

#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/http.hpp>

#include <process/ssl/flags.hpp>

#include <stout/option.hpp>
#include <stout/try.hpp>
#include <stout/uuid.hpp>

#include <mesos/mesos.hpp>

#include <mesos/authentication/secret_generator.hpp>

#ifdef USE_SSL_SOCKET
#include "authentication/executor/jwt_secret_generator.hpp"
#endif // USE_SSL_SOCKET

#include "common/validation.hpp"

#include "slave/container_daemon.hpp"

#include "tests/cluster.hpp"
#include "tests/mesos.hpp"

namespace http = process::http;

#ifdef USE_SSL_SOCKET
namespace openssl = process::network::openssl;
#endif // USE_SSL_SOCKET

using std::string;

using process::Future;
using process::Owned;
using process::PID;
using process::Promise;

using process::http::authentication::Principal;

#ifdef USE_SSL_SOCKET
using mesos::authentication::executor::JWTSecretGenerator;
#endif // USE_SSL_SOCKET

using mesos::internal::common::validation::validateSecret;

using mesos::internal::slave::ContainerDaemon;
using mesos::internal::slave::Slave;

using mesos::master::detector::MasterDetector;

namespace mesos {
namespace internal {
namespace tests {

class ContainerDaemonTest : public MesosTest {};


TEST_F(ContainerDaemonTest, RestartOnTermination)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Owned<SecretGenerator> secretGenerator;

  slave::Flags slaveFlags = CreateSlaveFlags();

#ifdef USE_SSL_SOCKET
  ASSERT_SOME(slaveFlags.jwt_secret_key);

  Try<string> jwtSecretKey = os::read(slaveFlags.jwt_secret_key.get());
  ASSERT_SOME(jwtSecretKey);

  secretGenerator.reset(new JWTSecretGenerator(jwtSecretKey.get()));
#endif // USE_SSL_SOCKET

  Future<Nothing> recover = FUTURE_DISPATCH(_, &Slave::__recover);

  Try<Owned<cluster::Slave>> slave = StartSlave(
      detector.get(),
      secretGenerator.get(),
      slaveFlags);

  ASSERT_SOME(slave);

  PID<Slave> slavePid = slave.get()->pid;

  // Ensure slave has finished recovery.
  AWAIT_READY(recover);

  string scheme = "http";

#ifdef USE_SSL_SOCKET
  if (openssl::flags().enabled) {
    scheme = "https";
  }
#endif // USE_SSL_SOCKET

  http::URL url(
      scheme,
      slavePid.address.ip,
      slavePid.address.port,
      strings::join("/", slavePid.id, "api/v1"));

  // NOTE: The current implicit authorization for creating standalone
  // containers is to check if the container ID prefix in the claims
  // of the principal is indeed a prefix of the container ID that is
  // specified in the API call.
  string containerIdPrefix = id::UUID::random().toString();

  ContainerID containerId;
  containerId.set_value(strings::join(
        "-",
        containerIdPrefix,
        id::UUID::random().toString()));

  Principal principal(
      None(),
      {{"cid_prefix", containerIdPrefix}});

  Option<string> authToken;
  if (secretGenerator.get() != nullptr) {
    Future<Secret> secret = secretGenerator->generate(principal);
    AWAIT_READY(secret);

    ASSERT_NONE(validateSecret(secret.get()));
    ASSERT_EQ(Secret::VALUE, secret->type());

    authToken = secret->value().data();
  }

  int runs = 0;
  Promise<Nothing> done;

  auto postStartHook = [&]() -> Future<Nothing> {
    runs++;
    return Nothing();
  };

  auto postStopHook = [&]() -> Future<Nothing> {
    if (runs >= 5) {
      done.set(Nothing());
    }
    return Nothing();
  };

  Try<Owned<ContainerDaemon>> daemon = ContainerDaemon::create(
      url,
      authToken,
      containerId,
      createCommandInfo("exit 0"),
      None(),
      None(),
      postStartHook,
      postStopHook);

  ASSERT_SOME(daemon);

  AWAIT_READY(done.future());

  Future<Nothing> wait = daemon.get()->wait();
  EXPECT_TRUE(wait.isPending());
}


#ifdef USE_SSL_SOCKET
// This test verifies that the container daemon will terminate itself
// if the agent operator API does not authorize the container launch.
TEST_F(ContainerDaemonTest, FailedAuthorization)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();

  ASSERT_SOME(slaveFlags.jwt_secret_key);

  Try<string> jwtSecretKey = os::read(slaveFlags.jwt_secret_key.get());
  ASSERT_SOME(jwtSecretKey);

  Owned<SecretGenerator> secretGenerator(
      new JWTSecretGenerator(jwtSecretKey.get()));

  Future<Nothing> recover = FUTURE_DISPATCH(_, &Slave::__recover);

  Try<Owned<cluster::Slave>> slave = StartSlave(
      detector.get(),
      secretGenerator.get(),
      slaveFlags);

  ASSERT_SOME(slave);

  PID<Slave> slavePid = slave.get()->pid;

  // Ensure slave has finished recovery.
  AWAIT_READY(recover);

  string scheme = "http";
  if (openssl::flags().enabled) {
    scheme = "https";
  }

  http::URL url(
      scheme,
      slavePid.address.ip,
      slavePid.address.port,
      strings::join("/", slavePid.id, "api/v1"));

  // NOTE: The current implicit authorization for creating standalone
  // containers is to check if the container ID prefix in the claims
  // of the principal is indeed a prefix of the container ID that is
  // specified in the API call.
  //
  // Using two random UUIDs here guarantees that one is not a prefix
  // of another. Therefore, the authorization will fail.
  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  Principal principal(
      None(),
      {{"cid_prefix", id::UUID::random().toString()}});

  Future<Secret> secret = secretGenerator->generate(principal);
  AWAIT_READY(secret);

  ASSERT_NONE(validateSecret(secret.get()));
  ASSERT_EQ(Secret::VALUE, secret->type());

  string authToken = secret->value().data();

  Try<Owned<ContainerDaemon>> daemon = ContainerDaemon::create(
      url,
      authToken,
      containerId,
      createCommandInfo("sleep 1000"),
      None(),
      None(),
      []() -> Future<Nothing> { return Nothing(); },
      []() -> Future<Nothing> { return Nothing(); });

  ASSERT_SOME(daemon);

  AWAIT_FAILED(daemon.get()->wait());
}
#endif // USE_SSL_SOCKET

} // namespace tests {
} // namespace internal {
} // namespace mesos {
