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

#include <string>

#include <mesos/authentication/authenticatee.hpp>
#include <mesos/authentication/authenticator.hpp>

#include <mesos/module/authenticatee.hpp>
#include <mesos/module/authenticator.hpp>

#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/pid.hpp>
#include <process/process.hpp>

#include <stout/gtest.hpp>

#include "authentication/cram_md5/authenticatee.hpp"
#include "authentication/cram_md5/authenticator.hpp"

#include "tests/mesos.hpp"
#include "tests/module.hpp"

using namespace mesos::tests;

using namespace process;

using std::string;

using testing::_;
using testing::Eq;

namespace mesos {
namespace cram_md5 {

template <typename T>
class CRAMMD5Authentication : public MesosTest {};

template <typename T, typename U>
class Authentication
{
public:
  typedef T TypeAuthenticatee;
  typedef U TypeAuthenticator;
};

// Test all authentication variants against each other.
// default authenticatee      <> default authenticator
// modularized authenticatee  <> default authenticator
// default authenticatee      <> modularized authenticator
// modularized authenticatee  <> modularized authenticator
typedef ::testing::Types<
  Authentication<CRAMMD5Authenticatee, CRAMMD5Authenticator>,
  Authentication<tests::Module<Authenticatee, TestCRAMMD5Authenticatee>,
                 CRAMMD5Authenticator>,
  Authentication<CRAMMD5Authenticatee,
                 tests::Module<Authenticator, TestCRAMMD5Authenticator>>,
  Authentication<tests::Module<Authenticatee, TestCRAMMD5Authenticatee>,
                 tests::Module<Authenticator, TestCRAMMD5Authenticator>>>
  AuthenticationTypes;

TYPED_TEST_CASE(CRAMMD5Authentication, AuthenticationTypes);

TYPED_TEST(CRAMMD5Authentication, success)
{
  // Launch a dummy process (somebody to send the AuthenticateMessage).
  UPID pid = spawn(new ProcessBase(), true);

  Credential credential1;
  credential1.set_principal("benh");
  credential1.set_secret("secret");

  Credentials credentials;
  Credential* credential2 = credentials.add_credentials();
  credential2->set_principal(credential1.principal());
  credential2->set_secret(credential1.secret());

  secrets::load(credentials);

  Future<Message> message =
    FUTURE_MESSAGE(Eq(AuthenticateMessage().GetTypeName()), _, _);

  Try<Authenticatee*> authenticatee = TypeParam::TypeAuthenticatee::create();
  CHECK_SOME(authenticatee);

  Future<bool> client =
    authenticatee.get()->authenticate(pid, UPID(), credential1);

  AWAIT_READY(message);

  Try<Authenticator*> authenticator = TypeParam::TypeAuthenticator::create();
  CHECK_SOME(authenticator);

  authenticator.get()->initialize(message.get().from);

  Future<Option<string>> principal = authenticator.get()->authenticate();

  AWAIT_EQ(true, client);
  AWAIT_READY(principal);
  EXPECT_SOME_EQ("benh", principal.get());

  terminate(pid);
  delete authenticator.get();
  delete authenticatee.get();
}


// Bad password should return an authentication failure.
TYPED_TEST(CRAMMD5Authentication, failed1)
{
  // Launch a dummy process (somebody to send the AuthenticateMessage).
  UPID pid = spawn(new ProcessBase(), true);

  Credential credential1;
  credential1.set_principal("benh");
  credential1.set_secret("secret");

  Credentials credentials;
  Credential* credential2 = credentials.add_credentials();
  credential2->set_principal(credential1.principal());
  credential2->set_secret("secret2");

  secrets::load(credentials);

  Future<Message> message =
    FUTURE_MESSAGE(Eq(AuthenticateMessage().GetTypeName()), _, _);

  Try<Authenticatee*> authenticatee = TypeParam::TypeAuthenticatee::create();
  CHECK_SOME(authenticatee);

  Future<bool> client =
    authenticatee.get()->authenticate(pid, UPID(), credential1);

  AWAIT_READY(message);

  Try<Authenticator*> authenticator = TypeParam::TypeAuthenticator::create();
  CHECK_SOME(authenticator);

  authenticator.get()->initialize(message.get().from);

  Future<Option<string>> server = authenticator.get()->authenticate();

  AWAIT_EQ(false, client);
  AWAIT_READY(server);
  EXPECT_NONE(server.get());

  terminate(pid);
  delete authenticator.get();
  delete authenticatee.get();
}


// No user should return an authentication failure.
TYPED_TEST(CRAMMD5Authentication, failed2)
{
  // Launch a dummy process (somebody to send the AuthenticateMessage).
  UPID pid = spawn(new ProcessBase(), true);

  Credential credential1;
  credential1.set_principal("benh");
  credential1.set_secret("secret");

  Credentials credentials;
  Credential* credential2 = credentials.add_credentials();
  credential2->set_principal("vinod");
  credential2->set_secret(credential1.secret());

  secrets::load(credentials);

  Future<Message> message =
    FUTURE_MESSAGE(Eq(AuthenticateMessage().GetTypeName()), _, _);

  Try<Authenticatee*> authenticatee = TypeParam::TypeAuthenticatee::create();
  CHECK_SOME(authenticatee);

  Future<bool> client =
    authenticatee.get()->authenticate(pid, UPID(), credential1);

  AWAIT_READY(message);

  Try<Authenticator*> authenticator = TypeParam::TypeAuthenticator::create();
  CHECK_SOME(authenticator);

  authenticator.get()->initialize(message.get().from);

  Future<Option<string>> server = authenticator.get()->authenticate();

  AWAIT_EQ(false, client);
  AWAIT_READY(server);
  EXPECT_NONE(server.get());

  terminate(pid);
  delete authenticator.get();
  delete authenticatee.get();
}


// This test verifies that the pending future returned by
// 'Authenticator::authenticate()' is properly failed when the Authenticator is
// destructed in the middle of authentication.
TYPED_TEST(CRAMMD5Authentication, AuthenticatorDestructionRace)
{
  // Launch a dummy process (somebody to send the AuthenticateMessage).
  UPID pid = spawn(new ProcessBase(), true);

  Credential credential1;
  credential1.set_principal("benh");
  credential1.set_secret("secret");

  Credentials credentials;
  Credential* credential2 = credentials.add_credentials();
  credential2->set_principal(credential1.principal());
  credential2->set_secret(credential1.secret());

  secrets::load(credentials);

  Future<Message> message =
    FUTURE_MESSAGE(Eq(AuthenticateMessage().GetTypeName()), _, _);

  Try<Authenticatee*> authenticatee = TypeParam::TypeAuthenticatee::create();
  CHECK_SOME(authenticatee);

  Future<bool> client =
    authenticatee.get()->authenticate(pid, UPID(), credential1);

  AWAIT_READY(message);

  Try<Authenticator*> authenticator = TypeParam::TypeAuthenticator::create();
  CHECK_SOME(authenticator);

  authenticator.get()->initialize(message.get().from);

  // Drop the AuthenticationStepMessage from authenticator to keep
  // the authentication from getting completed.
  Future<AuthenticationStepMessage> authenticationStepMessage =
    DROP_PROTOBUF(AuthenticationStepMessage(), _, _);

  Future<Option<string>> principal = authenticator.get()->authenticate();

  AWAIT_READY(authenticationStepMessage);

  // At this point 'AuthenticatorProcess::authenticate()' has been
  // executed and its promise associated with the promise returned
  // by 'Authenticator::authenticate()'.
  // Authentication should be pending.
  ASSERT_TRUE(principal.isPending());

  // Now delete the authenticator.
  delete authenticator.get();

  // The future should be failed at this point.
  AWAIT_FAILED(principal);

  terminate(pid);
  delete authenticatee.get();
}

} // namespace cram_md5 {
} // namespace mesos {
