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

#include <gtest/gtest.h>

#include <mesos/authentication/http/basic_authenticator_factory.hpp>

#include <mesos/authorizer/authorizer.hpp>

#include <process/future.hpp>
#include <process/http.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>

#include <stout/gtest.hpp>
#include <stout/try.hpp>

#include "master/master.hpp"

#include "tests/mesos.hpp"

using mesos::http::authentication::BasicAuthenticatorFactory;

using mesos::internal::master::Master;
using mesos::internal::slave::Slave;

using mesos::master::detector::MasterDetector;

using process::Owned;

using process::http::authorization::AuthorizationCallbacks;
using process::http::authentication::setAuthenticator;
using process::http::authentication::unsetAuthenticator;

namespace mesos {
namespace internal {
namespace tests {

class MetricsTest : public mesos::internal::tests::MesosTest
{
protected:
  void setBasicHttpAuthenticator(
      const std::string& realm,
      const Credentials& credentials)
  {
    Try<process::http::authentication::Authenticator*> authenticator =
      BasicAuthenticatorFactory::create(realm, credentials);

    ASSERT_SOME(authenticator);

    // Add this realm to the set of realms which will be unset during teardown.
    realms.insert(realm);

    // Pass ownership of the authenticator to libprocess.
    AWAIT_READY(setAuthenticator(
        realm,
        Owned<process::http::authentication::Authenticator>(
            authenticator.get())));
  }

  void TearDown() override
  {
    foreach (const std::string& realm, realms) {
      // We need to wait in order to ensure that the operation completes before
      // we leave `TearDown`. Otherwise, we may leak a mock object.
      AWAIT_READY(unsetAuthenticator(realm));
    }

    realms.clear();

    MesosTest::TearDown();
  }

private:
  hashset<std::string> realms;
};


// Tests that the `/metrics/snapshot` endpoint will reject unauthenticated
// requests when HTTP authentication is enabled on the master.
TEST_F(MetricsTest, MasterAuthenticationEnabled)
{
  Credentials credentials;
  credentials.add_credentials()->CopyFrom(DEFAULT_CREDENTIAL);

  // Create a basic HTTP authenticator with the specified credentials and set it
  // as the authenticator for `READONLY_HTTP_AUTHENTICATION_REALM`.
  setBasicHttpAuthenticator(READONLY_HTTP_AUTHENTICATION_REALM, credentials);

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Get the snapshot.
  process::UPID upid("metrics", process::address());

  process::Future<process::http::Response> response =
      process::http::get(upid, "snapshot");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(
      process::http::Unauthorized({}).status, response);
}


// Tests that the `/metrics/snapshot` endpoint will reject unauthenticated
// requests when HTTP authentication is enabled on the agent.
TEST_F(MetricsTest, AgentAuthenticationEnabled)
{
  Credentials credentials;
  credentials.add_credentials()->CopyFrom(DEFAULT_CREDENTIAL);

  // Create a basic HTTP authenticator with the specified credentials and set it
  // as the authenticator for `READONLY_HTTP_AUTHENTICATION_REALM`.
  setBasicHttpAuthenticator(READONLY_HTTP_AUTHENTICATION_REALM, credentials);

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> agent = StartSlave(detector.get());
  ASSERT_SOME(agent);

  // Get the snapshot.
  process::UPID upid("metrics", process::address());

  process::Future<process::http::Response> response =
      process::http::get(upid, "snapshot");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(
      process::http::Unauthorized({}).status, response);
}


// Tests that the `/metrics/snapshot` endpoint will reject unauthorized requests
// when authentication and authorization are enabled on the master.
TEST_F(MetricsTest, MasterAuthorizationEnabled)
{
  Credentials credentials;
  credentials.add_credentials()->CopyFrom(DEFAULT_CREDENTIAL);

  // Create a basic HTTP authenticator with the specified credentials and set it
  // as the authenticator for `READONLY_HTTP_AUTHENTICATION_REALM`.
  setBasicHttpAuthenticator(READONLY_HTTP_AUTHENTICATION_REALM, credentials);

  ACLs acls;

  // This ACL asserts that the principal of `DEFAULT_CREDENTIAL` can GET any
  // HTTP endpoints that are authorized with the `GetEndpoint` ACL.
  mesos::ACL::GetEndpoint* acl = acls.add_get_endpoints();
  acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
  acl->mutable_paths()->set_type(mesos::ACL::Entity::NONE);

  // Create a master.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.acls = acls;

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Get the snapshot.
  process::UPID upid("metrics", process::address());

  process::Future<process::http::Response> response = process::http::get(
      upid,
      "snapshot",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(
      process::http::Forbidden().status, response);
}


// Tests that the `/metrics/snapshot` endpoint will reject unauthorized requests
// when authentication and authorization are enabled on the agent.
TEST_F(MetricsTest, AgentAuthorizationEnabled)
{
  Credentials credentials;
  credentials.add_credentials()->CopyFrom(DEFAULT_CREDENTIAL);

  // Create a basic HTTP authenticator with the specified credentials and set it
  // as the authenticator for `READONLY_HTTP_AUTHENTICATION_REALM`.
  setBasicHttpAuthenticator(READONLY_HTTP_AUTHENTICATION_REALM, credentials);

  ACLs acls;

  // This ACL asserts that the principal of `DEFAULT_CREDENTIAL` can GET any
  // HTTP endpoints that are authorized with the `GetEndpoint` ACL.
  mesos::ACL::GetEndpoint* acl = acls.add_get_endpoints();
  acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
  acl->mutable_paths()->set_type(mesos::ACL::Entity::NONE);

  // Create an agent.
  slave::Flags agentFlags = CreateSlaveFlags();
  agentFlags.acls = acls;

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> agent = StartSlave(detector.get(), agentFlags);
  ASSERT_SOME(agent);

  // Get the snapshot.
  process::UPID upid("metrics", process::address());

  process::Future<process::http::Response> response = process::http::get(
      upid,
      "snapshot",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(
      process::http::Forbidden().status, response);
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
