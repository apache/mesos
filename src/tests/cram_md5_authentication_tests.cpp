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

using namespace mesos::internal::tests;

using namespace process;

using std::string;

using testing::_;
using testing::Eq;

namespace mesos {
namespace internal {
namespace cram_md5 {

template <typename T>
class CRAMMD5AuthenticationTest : public MesosTest {};

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
// TODO(josephw): Modules are not supported on Windows (MESOS-5994).
#ifndef __WINDOWS__
    Authentication<tests::Module<Authenticatee, TestCRAMMD5Authenticatee>,
                   CRAMMD5Authenticator>,
    Authentication<CRAMMD5Authenticatee,
                   tests::Module<Authenticator, TestCRAMMD5Authenticator>>,
    Authentication<tests::Module<Authenticatee, TestCRAMMD5Authenticatee>,
                   tests::Module<Authenticator, TestCRAMMD5Authenticator>>,
#endif // __WINDOWS__
    Authentication<CRAMMD5Authenticatee, CRAMMD5Authenticator>>
  AuthenticationTypes;

TYPED_TEST_CASE(CRAMMD5AuthenticationTest, AuthenticationTypes);

TYPED_TEST(CRAMMD5AuthenticationTest, Success)
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

  Future<Message> message =
    FUTURE_MESSAGE(Eq(AuthenticateMessage().GetTypeName()), _, _);

  Try<Authenticatee*> authenticatee = TypeParam::TypeAuthenticatee::create();
  ASSERT_SOME(authenticatee);

  Future<bool> client =
    authenticatee.get()->authenticate(pid, UPID(), credential1);

  AWAIT_READY(message);

  Try<Authenticator*> authenticator = TypeParam::TypeAuthenticator::create();
  ASSERT_SOME(authenticator);

  EXPECT_SOME(authenticator.get()->initialize(credentials));

  Future<Option<string>> principal =
    authenticator.get()->authenticate(message->from);

  AWAIT_TRUE(client);
  AWAIT_READY(principal);
  EXPECT_SOME_EQ("benh", principal.get());

  terminate(pid);

  delete authenticator.get();
  delete authenticatee.get();
}


// Bad password should return an authentication failure.
TYPED_TEST(CRAMMD5AuthenticationTest, Failed1)
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

  Future<Message> message =
    FUTURE_MESSAGE(Eq(AuthenticateMessage().GetTypeName()), _, _);

  Try<Authenticatee*> authenticatee = TypeParam::TypeAuthenticatee::create();
  ASSERT_SOME(authenticatee);

  Future<bool> client =
    authenticatee.get()->authenticate(pid, UPID(), credential1);

  AWAIT_READY(message);

  Try<Authenticator*> authenticator = TypeParam::TypeAuthenticator::create();
  ASSERT_SOME(authenticator);

  EXPECT_SOME(authenticator.get()->initialize(credentials));

  Future<Option<string>> server =
    authenticator.get()->authenticate(message->from);

  AWAIT_FALSE(client);
  AWAIT_READY(server);
  EXPECT_NONE(server.get());

  terminate(pid);

  delete authenticator.get();
  delete authenticatee.get();
}


// No user should return an authentication failure.
TYPED_TEST(CRAMMD5AuthenticationTest, Failed2)
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

  Future<Message> message =
    FUTURE_MESSAGE(Eq(AuthenticateMessage().GetTypeName()), _, _);

  Try<Authenticatee*> authenticatee = TypeParam::TypeAuthenticatee::create();
  ASSERT_SOME(authenticatee);

  Future<bool> client =
    authenticatee.get()->authenticate(pid, UPID(), credential1);

  AWAIT_READY(message);

  Try<Authenticator*> authenticator = TypeParam::TypeAuthenticator::create();
  ASSERT_SOME(authenticator);

  EXPECT_SOME(authenticator.get()->initialize(credentials));

  Future<Option<string>> server =
    authenticator.get()->authenticate(message->from);

  AWAIT_FALSE(client);
  AWAIT_READY(server);
  EXPECT_NONE(server.get());

  terminate(pid);

  delete authenticator.get();
  delete authenticatee.get();
}


// This test verifies that the pending future returned by
// 'Authenticator::authenticate()' is properly failed when the
// Authenticator Session is destroyed in the middle of authentication.
TYPED_TEST(CRAMMD5AuthenticationTest, AuthenticatorDestructionRace)
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

  Future<Message> message =
    FUTURE_MESSAGE(Eq(AuthenticateMessage().GetTypeName()), _, _);

  Try<Authenticatee*> authenticatee = TypeParam::TypeAuthenticatee::create();
  ASSERT_SOME(authenticatee);

  Future<bool> client =
    authenticatee.get()->authenticate(pid, UPID(), credential1);

  AWAIT_READY(message);

  Try<Authenticator*> authenticator = TypeParam::TypeAuthenticator::create();
  ASSERT_SOME(authenticator);

  EXPECT_SOME(authenticator.get()->initialize(credentials));

  // Drop the AuthenticationStepMessage from authenticator session to
  // keep the authentication from getting completed.
  Future<AuthenticationStepMessage> authenticationStepMessage =
    DROP_PROTOBUF(AuthenticationStepMessage(), _, _);

  Future<Option<string>> principal =
    authenticator.get()->authenticate(message->from);

  AWAIT_READY(authenticationStepMessage);

  // At this point 'AuthenticatorProcess::authenticate()' has been
  // executed.
  // Authentication should be pending.
  ASSERT_TRUE(principal.isPending());

  // Now delete the authenticator.
  delete authenticator.get();

  // The future should be failed at this point.
  AWAIT_FAILED(principal);

  terminate(pid);
  delete authenticatee.get();
}


// This test verifies that a missing secret fails the authenticatee.
TYPED_TEST(CRAMMD5AuthenticationTest, AuthenticateeSecretMissing)
{
  Credential credential;
  credential.set_principal("benh");

  Try<Authenticatee*> authenticatee = TypeParam::TypeAuthenticatee::create();
  ASSERT_SOME(authenticatee);

  Future<bool> future =
    authenticatee.get()->authenticate(UPID(), UPID(), credential);

  AWAIT_FALSE(future);

  delete authenticatee.get();
}

} // namespace cram_md5 {
} // namespace internal {
} // namespace mesos {
