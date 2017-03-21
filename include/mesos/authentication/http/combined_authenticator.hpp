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

#ifndef __MESOS_AUTHENTICATION_HTTP_COMBINED_AUTHENTICATOR_HPP__
#define __MESOS_AUTHENTICATION_HTTP_COMBINED_AUTHENTICATOR_HPP__

#include <string>
#include <vector>

#include <mesos/mesos.hpp>

#include <process/authenticator.hpp>
#include <process/future.hpp>
#include <process/http.hpp>
#include <process/owned.hpp>

#include <stout/hashset.hpp>

namespace mesos {
namespace http {
namespace authentication {

class CombinedAuthenticatorProcess;


/**
 * An authenticator which holds references to multiple authenticators.
 */
class CombinedAuthenticator
  : public process::http::authentication::Authenticator
{
public:
  CombinedAuthenticator(
      const std::string& realm,
      std::vector<process::Owned<
        process::http::authentication::Authenticator>>&& authenticators);

  /**
   * Non-copyable to protect the ownership of the installed authenticators.
   */
  CombinedAuthenticator(const CombinedAuthenticator&) = delete;

  ~CombinedAuthenticator() override;

  /**
   * Authenticates using the installed authenticators. When called, the
   * `authenticate()` method of each authenticator is called serially, and the
   * first successful result is returned. If all authentication attempts fail,
   * the failed results are combined as follows:
   *
   * - If any results are Unauthorized, then the Unauthorized results are merged
   *   and returned.
   * - If no Unauthorized results are found, then multiple Forbidden results
   *   may be present. The Forbidden results are combined and returned.
   * - If no Forbidden results are found, then failed futures may be present.
   *   Their error messages are combined and returned in a failed future.
   *
   * Below are examples illustrating various combinations of authentication
   * results when two authenticators are installed. In these examples, the
   * `scheme()` method of one authenticator returns "Basic". The other returns
   * "Bearer". These schemes are used when combining response bodies.
   *
   * Both authentication results are Unauthorized:
   *   - First result:
   *                   Status code: 401
   *     'WWW-Authenticate' header: 'Basic realm="mesos"'
   *                 Response body: 'Incorrect credentials'
   *   - Second result:
   *                   Status code: 401
   *     'WWW-Authenticate' header: 'Bearer realm="mesos"'
   *                 Response body: 'Invalid token'
   *
   *   - Returned result:
   *                   Status code: 401
   *     'WWW-Authenticate' header: 'Basic realm="mesos",Bearer realm="mesos"'
   *                 Response body: '"Basic" authenticator returned:\n'
   *                                'Incorrect credentials\n\n'
   *                                '"Bearer" authenticator returned:\n'
   *                                'Invalid token'
   *
   * One Unauthorized result and one Forbidden:
   *   - First result:
   *                   Status code: 401
   *     'WWW-Authenticate' header: 'Basic realm="mesos"'
   *                 Response body: 'Incorrect credentials'
   *   - Second result:
   *                   Status code: 403
   *                 Response body: 'Not authorized'
   *
   *   - Returned result:
   *                   Status code: 401
   *     'WWW-Authenticate' header: 'Basic realm="mesos"'
   *                 Response body: 'Incorrect credentials'
   *
   * Both authentication results are Forbidden:
   *   - First result:
   *                   Status code: 403
   *                 Response body: 'Basic: not authorized'
   *   - Second result:
   *                   Status code: 403
   *                 Response body: 'Bearer: not authorized'
   *
   *   - Returned result:
   *                   Status code: 403
   *                 Response body: '"Basic" authenticator returned:\n'
   *                                'Basic: not authorized\n\n'
   *                                '"Bearer" authenticator returned:\n'
   *                                'Bearer: not authorized'
   */
  process::Future<process::http::authentication::AuthenticationResult>
    authenticate(const process::http::Request& request) override;

  /**
   * Returns the authentication schemes offered by the
   * installed authenticators, separated by whitespace.
   */
  std::string scheme() const override;

private:
  process::Owned<CombinedAuthenticatorProcess> process;
  hashset<std::string> schemes;
};

} // namespace authentication {
} // namespace http {
} // namespace mesos {

#endif // __MESOS_AUTHENTICATION_HTTP_COMBINED_AUTHENTICATOR_HPP__
