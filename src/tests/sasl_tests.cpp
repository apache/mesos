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

#include "sasl/authenticatee.hpp"
#include "sasl/authenticator.hpp"

using namespace process;

using std::map;
using std::string;

using testing::_;
using testing::Eq;

namespace mesos {
namespace internal {
namespace sasl {

TEST(SASL, success)
{
  // Set up secrets.
  map<string, string> secrets;
  secrets["benh"] = "secret";
  sasl::secrets::load(secrets);

  // Launch a dummy process (somebody to send the AuthenticateMessage).
  UPID pid = spawn(new ProcessBase(), true);

  Credential credential;
  credential.set_principal("benh");
  credential.set_secret("secret");

  Authenticatee authenticatee(credential, UPID());

  Future<Message> message =
    FUTURE_MESSAGE(Eq(AuthenticateMessage().GetTypeName()), _, _);

  Future<bool> client = authenticatee.authenticate(pid);

  AWAIT_READY(message);

  Authenticator authenticator(message.get().from);

  Future<Option<string> > principal = authenticator.authenticate();

  AWAIT_EQ(true, client);
  AWAIT_READY(principal);
  EXPECT_SOME_EQ("benh", principal.get());

  terminate(pid);
}


// Bad password should return an authentication failure.
TEST(SASL, failed1)
{
  // Set up secrets.
  map<string, string> secrets;
  secrets["benh"] = "secret1";
  sasl::secrets::load(secrets);

  // Launch a dummy process (somebody to send the AuthenticateMessage).
  UPID pid = spawn(new ProcessBase(), true);

  Credential credential;
  credential.set_principal("benh");
  credential.set_secret("secret");

  Authenticatee authenticatee(credential, UPID());

  Future<Message> message =
    FUTURE_MESSAGE(Eq(AuthenticateMessage().GetTypeName()), _, _);

  Future<bool> client = authenticatee.authenticate(pid);

  AWAIT_READY(message);

  Authenticator authenticator(message.get().from);

  Future<Option<string> > server = authenticator.authenticate();

  AWAIT_EQ(false, client);
  AWAIT_READY(server);
  EXPECT_NONE(server.get());

  terminate(pid);
}


// No user should return an authentication failure.
TEST(SASL, failed2)
{
  // Set up secrets.
  map<string, string> secrets;
  secrets["vinod"] = "secret";
  sasl::secrets::load(secrets);

  // Launch a dummy process (somebody to send the AuthenticateMessage).
  UPID pid = spawn(new ProcessBase(), true);

  Credential credential;
  credential.set_principal("benh");
  credential.set_secret("secret");

  Authenticatee authenticatee(credential, UPID());

  Future<Message> message =
    FUTURE_MESSAGE(Eq(AuthenticateMessage().GetTypeName()), _, _);

  Future<bool> client = authenticatee.authenticate(pid);

  AWAIT_READY(message);

  Authenticator authenticator(message.get().from);

  Future<Option<string> > server = authenticator.authenticate();

  AWAIT_EQ(false, client);
  AWAIT_READY(server);
  EXPECT_NONE(server.get());

  terminate(pid);
}

} // namespace sasl {
} // namespace internal {
} // namespace mesos {
