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

#ifndef __AUTHENTICATION_HTTP_BASIC_AUTHENTICATEE_HPP__
#define __AUTHENTICATION_HTTP_BASIC_AUTHENTICATEE_HPP__

#include <mesos/v1/mesos.hpp>

#include <mesos/authentication/http/authenticatee.hpp>

#include <process/future.hpp>
#include <process/http.hpp>

#include <stout/option.hpp>

namespace mesos {
namespace http {
namespace authentication {

class BasicAuthenticateeProcess; // Forward declaration.

/**
 * Authenticatee implementing the client side of basic HTTP
 * authentication.
 */
class BasicAuthenticatee : public Authenticatee
{
public:
  BasicAuthenticatee();

  ~BasicAuthenticatee() override;

  // Not copy-constructable.
  BasicAuthenticatee(const BasicAuthenticatee&) = delete;

  // Not copyable.
  BasicAuthenticatee& operator=(const BasicAuthenticatee&) = delete;

  std::string scheme() const override;

  process::Future<process::http::Request> authenticate(
      const process::http::Request& request,
      const Option<mesos::v1::Credential>& credential) override;

private:
  process::Owned<BasicAuthenticateeProcess> process_;
};

} // namespace authentication {
} // namespace http {
} // namespace mesos {

#endif // __AUTHENTICATION_HTTP_BASIC_AUTHENTICATEE_HPP__
