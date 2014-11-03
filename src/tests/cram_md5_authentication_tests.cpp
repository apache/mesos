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

#include <stout/gtest.hpp>

#include "authentication/cram_md5/authenticatee.hpp"
#include "authentication/cram_md5/authenticator.hpp"

#include "tests/mesos.hpp"

using namespace mesos::internal::tests;

using namespace process;

using std::map;
using std::string;

using testing::_;
using testing::Eq;

namespace mesos {
namespace internal {
namespace cram_md5 {

TEST(CRAMMD5Authentication, success)
{
  // Launch a dummy process (somebody to send the AuthenticateMessage).
  UPID pid = spawn(new ProcessBase(), true);

  Credential credential1;
  credential1.set_principal("benh");
  credential1.set_secret("secret");

  Authenticatee authenticatee(credential1, UPID());

  Future<Message> message =
    FUTURE_MESSAGE(Eq(AuthenticateMessage().GetTypeName()), _, _);

  Future<bool> client = authenticatee.authenticate(pid);

  AWAIT_READY(message);

  Credentials credentials;
  Credential* credential2 = credentials.add_credentials();
  credential2->set_principal(credential1.principal());
  credential2->set_secret(credential1.secret());

  CRAMMD5Authenticator authenticator;
  EXPECT_SOME(authenticator.initialize(message.get().from, credentials));

  Future<Option<string>> principal = authenticator.authenticate();

  AWAIT_EQ(true, client);
  AWAIT_READY(principal);
  EXPECT_SOME_EQ("benh", principal.get());

  terminate(pid);
}


// Bad password should return an authentication failure.
TEST(CRAMMD5Authentication, failed1)
{
  // Launch a dummy process (somebody to send the AuthenticateMessage).
  UPID pid = spawn(new ProcessBase(), true);

  Credential credential1;
  credential1.set_principal("benh");
  credential1.set_secret("secret");

  Authenticatee authenticatee(credential1, UPID());

  Future<Message> message =
    FUTURE_MESSAGE(Eq(AuthenticateMessage().GetTypeName()), _, _);

  Future<bool> client = authenticatee.authenticate(pid);

  AWAIT_READY(message);

  Credentials credentials;
  Credential* credential2 = credentials.add_credentials();
  credential2->set_principal(credential1.principal());
  credential2->set_secret("secret2");

  CRAMMD5Authenticator authenticator;
  EXPECT_SOME(authenticator.initialize(message.get().from, credentials));

  Future<Option<string>> server = authenticator.authenticate();

  AWAIT_EQ(false, client);
  AWAIT_READY(server);
  EXPECT_NONE(server.get());

  terminate(pid);
}


// No user should return an authentication failure.
TEST(CRAMMD5Authentication, failed2)
{
  // Launch a dummy process (somebody to send the AuthenticateMessage).
  UPID pid = spawn(new ProcessBase(), true);

  Credential credential1;
  credential1.set_principal("benh");
  credential1.set_secret("secret");

  Authenticatee authenticatee(credential1, UPID());

  Future<Message> message =
    FUTURE_MESSAGE(Eq(AuthenticateMessage().GetTypeName()), _, _);

  Future<bool> client = authenticatee.authenticate(pid);

  AWAIT_READY(message);

  Credentials credentials;
  Credential* credential2 = credentials.add_credentials();
  credential2->set_principal("vinod");
  credential2->set_secret(credential1.secret());

  CRAMMD5Authenticator authenticator;
  EXPECT_SOME(authenticator.initialize(message.get().from, credentials));

  Future<Option<string>> server = authenticator.authenticate();

  AWAIT_EQ(false, client);
  AWAIT_READY(server);
  EXPECT_NONE(server.get());

  terminate(pid);
}


// This test verifies that the pending future returned by
// 'Authenticator::authenticate()' is properly failed when the Authenticator is
// destructed in the middle of authentication.
TEST(CRAMMD5Authentication, AuthenticatorDestructionRace)
{
  // Launch a dummy process (somebody to send the AuthenticateMessage).
  UPID pid = spawn(new ProcessBase(), true);

  Credential credential1;
  credential1.set_principal("benh");
  credential1.set_secret("secret");

  Authenticatee authenticatee(credential1, UPID());

  Future<Message> message =
    FUTURE_MESSAGE(Eq(AuthenticateMessage().GetTypeName()), _, _);

  Future<bool> client = authenticatee.authenticate(pid);

  AWAIT_READY(message);

  Credentials credentials;
  Credential* credential2 = credentials.add_credentials();
  credential2->set_principal(credential1.principal());
  credential2->set_secret(credential1.secret());

  CRAMMD5Authenticator* authenticator = new CRAMMD5Authenticator();
  EXPECT_SOME(authenticator->initialize(message.get().from, credentials));

  // Drop the AuthenticationStepMessage from authenticator to keep
  // the authentication from getting completed.
  Future<AuthenticationStepMessage> authenticationStepMessage =
    DROP_PROTOBUF(AuthenticationStepMessage(), _, _);

  Future<Option<string>> principal = authenticator->authenticate();

  AWAIT_READY(authenticationStepMessage);

  // At this point 'AuthenticatorProcess::authenticate()' has been
  // executed and its promise associated with the promise returned
  // by 'Authenticator::authenticate()'.
  // Authentication should be pending.
  ASSERT_TRUE(principal.isPending());

  // Now delete the authenticator.
  delete authenticator;

  // The future should be failed at this point.
  AWAIT_FAILED(principal);

  terminate(pid);
}


// Missing credentials should fail the initializing.
TEST(CRAMMD5Authentication, AuthenticatorCredentialsMissing)
{
  CRAMMD5Authenticator authenticator;
  EXPECT_ERROR(authenticator.initialize(UPID(), None()));
}

} // namespace cram_md5 {
} // namespace internal {
} // namespace mesos {
