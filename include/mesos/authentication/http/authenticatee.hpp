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

#ifndef __MESOS_AUTHENTICATION_HTTP_AUTHENTICATEE_HPP__
#define __MESOS_AUTHENTICATION_HTTP_AUTHENTICATEE_HPP__

#include <mesos/v1/mesos.hpp>

#include <process/future.hpp>
#include <process/http.hpp>

#include <stout/option.hpp>

namespace mesos {
namespace http {
namespace authentication {

/**
 * An abstraction enabling any HTTP API consumer to authenticate.
 */
class Authenticatee
{
public:
  virtual ~Authenticatee() = default;

  /**
   * Reset the authenticatee to its initial state.
   *
   * Allows the implementation to invalidate possibly cached state.
   * This is useful if for example token-based authentication is
   * performed and the authenticator signaled an expired token.
   */
  virtual void reset() {};

  /**
   * Name of the authentication scheme implemented.
   *
   * @return Authentication scheme.
   */
  virtual std::string scheme() const = 0;

  /**
   * Create an HTTP request for authentication.
   *
   * Used for mutating a provided `Request` with any means of
   * authentication-related headers or other additions and changes.
   *
   * @param request The HTTP payload intended to be sent to an
   *     authenticated endpoint.
   * @param credential The principal and secret optionally used to
   *     create the authentication request.
   * @return The mutated HTTP request object containing all information
   *     needed for authenticating.
   */
  virtual process::Future<process::http::Request> authenticate(
      const process::http::Request& request,
      const Option<mesos::v1::Credential>& credential) = 0;
};

} // namespace authentication {
} // namespace http {
} // namespace mesos {

#endif // __MESOS_AUTHENTICATION_HTTP_AUTHENTICATEE_HPP__
