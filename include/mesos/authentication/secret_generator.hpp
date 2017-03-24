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

#ifndef __MESOS_AUTHENTICATION_SECRET_GENERATOR_HPP__
#define __MESOS_AUTHENTICATION_SECRET_GENERATOR_HPP__

#include <mesos/mesos.hpp>

#include <process/future.hpp>
#include <process/http.hpp>

namespace mesos {

/**
 * The SecretGenerator interface represents a mechanism to create a secret
 * from a principal. The secret is meant to contain a token (for example,
 * an HTTP 'Authorization' header) that can be used to authenticate. If
 * used that way, an authenticator will yield the same principal that
 * was passed to the `generate` method of this module.
 */
class SecretGenerator
{
public:
  virtual ~SecretGenerator() {}

  virtual process::Future<Secret> generate(
      const process::http::authentication::Principal& principal) = 0;
};

} // namespace mesos {

#endif // __MESOS_AUTHENTICATION_SECRET_GENERATOR_HPP__
