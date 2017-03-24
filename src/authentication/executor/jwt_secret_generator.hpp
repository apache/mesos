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

#ifndef __MESOS_AUTHENTICATION_EXECUTOR_JWT_SECRET_GENERATOR_HPP__
#define __MESOS_AUTHENTICATION_EXECUTOR_JWT_SECRET_GENERATOR_HPP__

#include <string>

#include <mesos/authentication/secret_generator.hpp>

#include <process/authenticator.hpp>
#include <process/future.hpp>

namespace mesos {
namespace authentication {
namespace executor {

/**
 * Creates a VALUE-type secret containing a JWT. When the secret is
 * presented to the 'JWTAuthenticator' module, the authenticator will
 * return the principal which is provided here as an argument.
 */
class JWTSecretGenerator : public SecretGenerator
{
public:
  JWTSecretGenerator(const std::string& secret);

  ~JWTSecretGenerator() override;

  process::Future<Secret> generate(
      const process::http::authentication::Principal& principal)
    override;

private:
  std::string secret_;
};

} // namespace executor {
} // namespace authentication {
} // namespace mesos {

#endif // __MESOS_AUTHENTICATION_EXECUTOR_JWT_SECRET_GENERATOR_HPP__
