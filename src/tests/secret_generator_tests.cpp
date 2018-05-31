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
// limitations under the License

#include <gtest/gtest.h>

#include <map>
#include <string>

#include <mesos/mesos.hpp>

#include <process/authenticator.hpp>
#include <process/future.hpp>
#include <process/gtest.hpp>
#include <process/jwt.hpp>

#include <stout/gtest.hpp>
#include <stout/json.hpp>

#include "authentication/executor/jwt_secret_generator.hpp"

namespace mesos {
namespace internal {
namespace tests {

using mesos::authentication::executor::JWTSecretGenerator;

using process::Future;

using process::http::authentication::JWT;
using process::http::authentication::JWTError;
using process::http::authentication::Principal;

using std::string;


TEST(JWTSecretGeneratorTest, Generate)
{
  const string secret = "secret";

  JWTSecretGenerator generator(secret);

  Principal principal(Option<string>::none());
  principal.claims["sub"] = "user";
  principal.claims["foo"] = "bar";

  const Future<Secret> token = generator.generate(principal);

  AWAIT_READY(token);

  EXPECT_TRUE(token->has_value());
  EXPECT_TRUE(token->value().has_data());

  const Try<JWT, JWTError> jwt = JWT::parse(token->value().data(), secret);

  EXPECT_SOME(jwt);

  Result<JSON::String> sub = jwt->payload.at<JSON::String>("sub");

  EXPECT_SOME_EQ("user", sub);

  Result<JSON::String> foo = jwt->payload.at<JSON::String>("foo");

  EXPECT_SOME_EQ("bar", foo);
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
