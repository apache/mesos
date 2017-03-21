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

namespace authentication = process::http::authentication;

using mesos::http::authentication::BasicAuthenticatorFactory;

using mesos::internal::master::Master;
using mesos::internal::slave::Slave;

using mesos::master::detector::MasterDetector;

using process::Owned;

using process::http::authorization::AuthorizationCallbacks;

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
    Try<authentication::Authenticator*> authenticator =
      BasicAuthenticatorFactory::create(realm, credentials);

    ASSERT_SOME(authenticator);

    // Add this realm to the set of realms which will be unset during teardown.
    realms.insert(realm);

    // Pass ownership of the authenticator to libprocess.
    AWAIT_READY(authentication::setAuthenticator(
        realm,
        Owned<authentication::Authenticator>(authenticator.get())));
  }

  virtual void TearDown()
  {
    foreach (const std::string& realm, realms) {
      // We need to wait in order to ensure that the operation completes before
      // we leave `TearDown`. Otherwise, we may leak a mock object.
      AWAIT_READY(authentication::unsetAuthenticator(realm));
    }

    realms.clear();

    MesosTest::TearDown();
  }

private:
  hashset<std::string> realms;
};


TEST_F(MetricsTest, Master)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Get the snapshot.
  process::UPID upid("metrics", process::address());

  process::Future<process::http::Response> response =
      process::http::get(upid, "snapshot");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(process::http::OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
  ASSERT_SOME(parse);

  JSON::Object stats = parse.get();

  EXPECT_EQ(1u, stats.values.count("master/uptime_secs"));

  EXPECT_EQ(1u, stats.values.count("master/elected"));

  EXPECT_EQ(1u, stats.values.count("master/slaves_connected"));
  EXPECT_EQ(1u, stats.values.count("master/slaves_disconnected"));
  EXPECT_EQ(1u, stats.values.count("master/slaves_active"));
  EXPECT_EQ(1u, stats.values.count("master/slaves_inactive"));

  EXPECT_EQ(1u, stats.values.count("master/frameworks_connected"));
  EXPECT_EQ(1u, stats.values.count("master/frameworks_disconnected"));
  EXPECT_EQ(1u, stats.values.count("master/frameworks_active"));
  EXPECT_EQ(1u, stats.values.count("master/frameworks_inactive"));

  EXPECT_EQ(1u, stats.values.count("master/outstanding_offers"));

  EXPECT_EQ(1u, stats.values.count("master/tasks_staging"));
  EXPECT_EQ(1u, stats.values.count("master/tasks_starting"));
  EXPECT_EQ(1u, stats.values.count("master/tasks_running"));
  EXPECT_EQ(1u, stats.values.count("master/tasks_killing"));
  EXPECT_EQ(1u, stats.values.count("master/tasks_finished"));
  EXPECT_EQ(1u, stats.values.count("master/tasks_failed"));
  EXPECT_EQ(1u, stats.values.count("master/tasks_killed"));
  EXPECT_EQ(1u, stats.values.count("master/tasks_lost"));
  EXPECT_EQ(1u, stats.values.count("master/tasks_error"));

  EXPECT_EQ(1u, stats.values.count("master/dropped_messages"));

  // Messages from schedulers.
  EXPECT_EQ(1u, stats.values.count("master/messages_register_framework"));
  EXPECT_EQ(1u, stats.values.count("master/messages_reregister_framework"));
  EXPECT_EQ(1u, stats.values.count("master/messages_unregister_framework"));
  EXPECT_EQ(1u, stats.values.count("master/messages_deactivate_framework"));
  EXPECT_EQ(1u, stats.values.count("master/messages_kill_task"));
  EXPECT_EQ(1u, stats.values.count(
      "master/messages_status_update_acknowledgement"));
  EXPECT_EQ(1u, stats.values.count("master/messages_resource_request"));
  EXPECT_EQ(1u, stats.values.count("master/messages_launch_tasks"));
  EXPECT_EQ(1u, stats.values.count("master/messages_decline_offers"));
  EXPECT_EQ(1u, stats.values.count("master/messages_revive_offers"));
  EXPECT_EQ(1u, stats.values.count("master/messages_suppress_offers"));
  EXPECT_EQ(1u, stats.values.count("master/messages_reconcile_tasks"));
  EXPECT_EQ(1u, stats.values.count("master/messages_framework_to_executor"));

  // Messages from slaves.
  EXPECT_EQ(1u, stats.values.count("master/messages_register_slave"));
  EXPECT_EQ(1u, stats.values.count("master/messages_reregister_slave"));
  EXPECT_EQ(1u, stats.values.count("master/messages_unregister_slave"));
  EXPECT_EQ(1u, stats.values.count("master/messages_status_update"));
  EXPECT_EQ(1u, stats.values.count("master/messages_exited_executor"));

  // Messages from both schedulers and slaves.
  EXPECT_EQ(1u, stats.values.count("master/messages_authenticate"));

  EXPECT_EQ(1u, stats.values.count(
      "master/valid_framework_to_executor_messages"));
  EXPECT_EQ(1u, stats.values.count(
      "master/invalid_framework_to_executor_messages"));

  EXPECT_EQ(1u, stats.values.count("master/valid_status_updates"));
  EXPECT_EQ(1u, stats.values.count("master/invalid_status_updates"));

  EXPECT_EQ(1u, stats.values.count(
      "master/valid_status_update_acknowledgements"));
  EXPECT_EQ(1u, stats.values.count(
      "master/invalid_status_update_acknowledgements"));

  EXPECT_EQ(1u, stats.values.count("master/recovery_slave_removals"));

  EXPECT_EQ(1u, stats.values.count("master/event_queue_messages"));
  EXPECT_EQ(1u, stats.values.count("master/event_queue_dispatches"));
  EXPECT_EQ(1u, stats.values.count("master/event_queue_http_requests"));

  EXPECT_EQ(1u, stats.values.count("master/cpus_total"));
  EXPECT_EQ(1u, stats.values.count("master/cpus_used"));
  EXPECT_EQ(1u, stats.values.count("master/cpus_percent"));

  EXPECT_EQ(1u, stats.values.count("master/mem_total"));
  EXPECT_EQ(1u, stats.values.count("master/mem_used"));
  EXPECT_EQ(1u, stats.values.count("master/mem_percent"));

  EXPECT_EQ(1u, stats.values.count("master/disk_total"));
  EXPECT_EQ(1u, stats.values.count("master/disk_used"));
  EXPECT_EQ(1u, stats.values.count("master/disk_percent"));

  EXPECT_EQ(1u, stats.values.count("registrar/queued_operations"));
  EXPECT_EQ(1u, stats.values.count("registrar/registry_size_bytes"));

  EXPECT_EQ(1u, stats.values.count("registrar/state_fetch_ms"));
  EXPECT_EQ(1u, stats.values.count("registrar/state_store_ms"));
}


TEST_F(MetricsTest, Slave)
{
  // TODO(dhamon): https://issues.apache.org/jira/browse/MESOS-2134 to allow
  // only a Slave to be started.
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  // Get the snapshot.
  process::UPID upid("metrics", process::address());

  process::Future<process::http::Response> response =
      process::http::get(upid, "snapshot");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(process::http::OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
  ASSERT_SOME(parse);

  JSON::Object stats = parse.get();

  EXPECT_EQ(1u, stats.values.count("slave/uptime_secs"));
  EXPECT_EQ(1u, stats.values.count("slave/registered"));

  EXPECT_EQ(1u, stats.values.count("slave/recovery_errors"));

  EXPECT_EQ(1u, stats.values.count("slave/frameworks_active"));

  EXPECT_EQ(1u, stats.values.count("slave/tasks_staging"));
  EXPECT_EQ(1u, stats.values.count("slave/tasks_starting"));
  EXPECT_EQ(1u, stats.values.count("slave/tasks_running"));
  EXPECT_EQ(1u, stats.values.count("slave/tasks_killing"));
  EXPECT_EQ(1u, stats.values.count("slave/tasks_finished"));
  EXPECT_EQ(1u, stats.values.count("slave/tasks_failed"));
  EXPECT_EQ(1u, stats.values.count("slave/tasks_killed"));
  EXPECT_EQ(1u, stats.values.count("slave/tasks_lost"));

  EXPECT_EQ(1u, stats.values.count("slave/executors_registering"));
  EXPECT_EQ(1u, stats.values.count("slave/executors_running"));
  EXPECT_EQ(1u, stats.values.count("slave/executors_terminating"));
  EXPECT_EQ(1u, stats.values.count("slave/executors_terminated"));

  EXPECT_EQ(1u, stats.values.count("slave/valid_status_updates"));
  EXPECT_EQ(1u, stats.values.count("slave/invalid_status_updates"));

  EXPECT_EQ(1u, stats.values.count("slave/valid_framework_messages"));
  EXPECT_EQ(1u, stats.values.count("slave/invalid_framework_messages"));

  EXPECT_EQ(1u, stats.values.count("slave/container_launch_errors"));

  EXPECT_EQ(1u, stats.values.count("slave/cpus_total"));
  EXPECT_EQ(1u, stats.values.count("slave/cpus_used"));
  EXPECT_EQ(1u, stats.values.count("slave/cpus_percent"));

  EXPECT_EQ(1u, stats.values.count("slave/mem_total"));
  EXPECT_EQ(1u, stats.values.count("slave/mem_used"));
  EXPECT_EQ(1u, stats.values.count("slave/mem_percent"));

  EXPECT_EQ(1u, stats.values.count("slave/disk_total"));
  EXPECT_EQ(1u, stats.values.count("slave/disk_used"));
  EXPECT_EQ(1u, stats.values.count("slave/disk_percent"));
}


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
