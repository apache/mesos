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

#ifndef __PROVISIONER_DOCKER_TOKEN_MANAGER_HPP__
#define __PROVISIONER_DOCKER_TOKEN_MANAGER_HPP__

#include <functional>
#include <string>

#include <stout/base64.hpp>
#include <stout/duration.hpp>
#include <stout/hashmap.hpp>
#include <stout/strings.hpp>

#include <process/future.hpp>
#include <process/http.hpp>
#include <process/process.hpp>
#include <process/time.hpp>

namespace mesos {
namespace internal {
namespace slave {
namespace docker {
namespace registry {


/**
 * Encapsulates JSON Web Token.
 *
 * Reference: https://tools.ietf.org/html/rfc7519.
 */
struct Token
{
  /**
   * Factory method for Token object.
   *
   * Parses the raw token string and validates for token's expiration.
   *
   * @returns Token if parsing and validation succeeds.
   *          Error if parsing or validation fails.
   */
  static Try<Token> create(const std::string& rawString);

  /**
   * Compares token's expiration time(expressed in seconds) with current time.
   *
   * @returns True if token's expiration time is greater than current time.
   *          False if token's expiration time is less than or equal to current
   *          time.
   */
  bool isExpired() const;

  /**
   * Validates the token if its "exp" "nbf" values are in range.
   *
   * @returns True if current time is within token's "exp" and "nbf" values.
   *          False if current time is not within token's "exp" and "nbf"
   *          values.
   */
  bool isValid() const;

  const std::string raw;
  const JSON::Object header;
  const JSON::Object claims;
  // TODO(jojy): Add signature information.

private:
  Token(
      const std::string& raw,
      const JSON::Object& headerJson,
      const JSON::Object& claimsJson,
      const Option<process::Time>& expireTime,
      const Option<process::Time>& notBeforeTime);

  static Result<process::Time> getTimeValue(
      const JSON::Object& object,
      const std::string& key);

  const Option<process::Time> expiration;
  const Option<process::Time> notBefore;
};


// Forward declaration.
class TokenManagerProcess;


/**
 *  Acquires and manages docker registry tokens. It keeps the tokens in its
 *  cache to server any future request for the same token.
 *  The cache grows unbounded.
 *  TODO(jojy): The cache can be optimized to prune based on the expiry time of
 *  the token and server's issue time.
 */
class TokenManager
{
public:
  /**
   * Factory method for creating TokenManager object.
   *
   * TokenManager and registry authorization realm has a 1:1 relationship.
   *
   * @param realm URL of the authorization server from where token will be
   *     requested by this TokenManager.
   * @returns Owned<TokenManager> if success.
   *          Error on failure.
   */
  static Try<process::Owned<TokenManager>> create(
      const process::http::URL& realm);

  /**
   * Returns JSON Web Token from cache or from remote server using "Basic
   * authorization".
   *
   * @param service Name of the service that hosts the resource for which
   *     token is being requested.
   * @param scope unique scope returned by the 401 Unauthorized response
   *     from the registry.
   * @param account Name of the account which the client is acting as.
   * @param user base64 encoded userid for basic authorization.
   * @param password base64 encoded password for basic authorization.
   * @returns Token struct that encapsulates JSON Web Token.
   */
  process::Future<Token> getToken(
      const std::string& service,
      const std::string& scope,
      const Option<std::string>& account,
      const std::string& user,
      const Option<std::string>& password);

  /**
   * Returns JSON Web Token from cache or from remote server using "TLS/Cert"
   * based authorization.
   *
   * @param service Name of the service that hosts the resource for which
   *     token is being requested.
   * @param scope unique scope returned by the 401 Unauthorized response
   *     from the registry.
   * @param account Name of the account which the client is acting as.
   * @returns Token struct that encapsulates JSON Web Token.
   */
  process::Future<Token> getToken(
      const std::string& service,
      const std::string& scope,
      const Option<std::string>& account);

  ~TokenManager();

private:
  TokenManager(process::Owned<TokenManagerProcess>& process);

  TokenManager(const TokenManager&) = delete;
  TokenManager& operator=(const TokenManager&) = delete;

  process::Owned<TokenManagerProcess> process_;
};

} // namespace registry {
} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __PROVISIONER_DOCKER_TOKEN_MANAGER_HPP__
