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

#include "jwt_secret_generator.hpp"

#include <process/jwt.hpp>

#include <stout/json.hpp>
#include <stout/stringify.hpp>

namespace mesos {
namespace authentication {
namespace executor {

using process::Failure;
using process::Future;

using process::http::authentication::JWT;
using process::http::authentication::JWTError;
using process::http::authentication::Principal;

using std::string;


JWTSecretGenerator::JWTSecretGenerator(const string& secret)
  : secret_(secret) {}


JWTSecretGenerator::~JWTSecretGenerator() {}


Future<Secret> JWTSecretGenerator::generate(const Principal& principal)
{
  if (principal.value.isSome()) {
    return Failure("Principal has a value, but only claims are supported");
  }

  JSON::Object payload;

  foreachpair (const string& key, const string& value, principal.claims) {
    payload.values[key] = value;
  }

  Try<JWT, JWTError> jwt = JWT::create(payload, secret_);

  if (jwt.isError()) {
    return Failure("Error creating JWT: " + jwt.error().message);
  }

  Secret::Value value;
  value.set_data(stringify(jwt.get()));

  Secret result;
  result.set_type(Secret::VALUE);
  result.mutable_value()->CopyFrom(value);

  return result;
}

} // namespace executor {
} // namespace authentication {
} // namespace mesos {
