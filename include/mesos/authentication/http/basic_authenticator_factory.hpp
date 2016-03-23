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

#ifndef __MESOS_AUTHENTICATION_HTTP_AUTHENTICATOR_FACTORY_HPP__
#define __MESOS_AUTHENTICATION_HTTP_AUTHENTICATOR_FACTORY_HPP__

#include <string>

#include <mesos/mesos.hpp>

#include <process/authenticator.hpp>

#include <stout/try.hpp>
#include <stout/hashmap.hpp>

namespace mesos {
namespace http {
namespace authentication {

class BasicAuthenticatorFactory {
public:
  ~BasicAuthenticatorFactory() {}

  /**
   * Creates a basic HTTP authenticator from module parameters. Two keys may be
   * specified in the parameters: `authentication_realm` (required) and
   * `credentials` (optional). Credentials must be provided as a string which
   * represents a JSON array, similar to the format of the `--credentials`
   * command-line flag.
   *
   * Example parameters as JSON:
   * {
   *   "parameters": [
   *     {
   *       "key": "authentication_realm",
   *       "value": "tatooine"
   *     },
   *     {
   *       "key": "credentials",
   *       "value": "[
   *         {
   *           \"principal\": \"luke\",
   *           \"secret\": \"skywalker\"
   *         }
   *       ]"
   *     }
   *   ]
   * }
   *
   * @param parameters The input parameters. This object may contain two keys:
   *     `authentication_realm` (required) and `credentials` (optional).
   * @return A `Try` containing a new authenticator if successful, or an
   *     `Error` if unsuccessful.
   */
  static Try<process::http::authentication::Authenticator*> create(
      const Parameters& parameters);

  /**
   * Creates a basic HTTP authenticator for the specified realm, initialized
   * with the provided credentials.
   *
   * @param realm The authentication realm associated with this authenticator.
   * @param credentials The credentials that this authenticator will use to
   *     evaluate authentication requests.
   * @return A `Try` containing a new authenticator if successful, or an
   *     `Error` if unsuccessful.
   */
  static Try<process::http::authentication::Authenticator*> create(
      const std::string& realm,
      const Credentials& credentials);

  /**
   * Creates a basic HTTP authenticator for the specified realm, initialized
   * with the provided credentials.
   *
   * @param realm The authentication realm associated with this authenticator.
   * @param credentials The credentials that this authenticator will use to
   *     evaluate authentication requests.
   * @return A `Try` containing a new authenticator if successful, or an
   *     `Error` if unsuccessful.
   */
  static Try<process::http::authentication::Authenticator*> create(
      const std::string& realm,
      const hashmap<std::string, std::string>& credentials);

protected:
  BasicAuthenticatorFactory() {}
};

} // namespace authentication {
} // namespace http {
} // namespace mesos {

#endif // __MESOS_AUTHENTICATION_HTTP_AUTHENTICATOR_FACTORY_HPP__
