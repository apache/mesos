// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License

#ifndef __PROCESS_AUTHENTICATOR_HPP__
#define __PROCESS_AUTHENTICATOR_HPP__

#include <iosfwd>
#include <string>

#include <process/future.hpp>
#include <process/http.hpp>

#include <stout/hashmap.hpp>
#include <stout/option.hpp>

namespace process {
namespace http {
namespace authentication {

class BasicAuthenticatorProcess;
#ifdef USE_SSL_SOCKET
class JWTAuthenticatorProcess;
#endif // USE_SSL_SOCKET

/**
 * Contains information associated with an authenticated principal.
 *
 * At least one of the following two members must be set:
 *   `value` : Optional string which is used to identify this principal.
 *   `claims`: Map containing key-value pairs associated with this principal.
 */
struct Principal
{
  Principal() = delete;

  Principal(const Option<std::string>& _value)
    : value(_value) {}

  Principal(
      const Option<std::string>& _value,
      const hashmap<std::string, std::string>& _claims)
    : value(_value), claims(_claims) {}

  bool operator==(const Principal& that) const
  {
    return this->value == that.value && this->claims == that.claims;
  }

  bool operator==(const std::string& that) const
  {
    return this->value == that;
  }

  bool operator!=(const std::string& that) const
  {
    return !(*this == that);
  }

  Option<std::string> value;
  hashmap<std::string, std::string> claims;
};


std::ostream& operator<<(std::ostream& stream, const Principal& principal);


/**
 * Represents the result of authenticating a request.
 * An `AuthenticationResult` can represent one of the
 * following states:
 *
 * 1. The request was successfully authenticated:
 *    `principal` is set.
 * 2. The request was not authenticated and a new
 *    challenge is issued: `unauthorized` is set.
 * 3. The request was not authenticated but no new
 *    challenge is issued: `forbidden` is set.
 *
 * Exactly one of the member variables must be set.
 * Setting more than one or not setting any is an error.
 */
struct AuthenticationResult
{
  Option<Principal> principal;
  Option<Unauthorized> unauthorized;
  Option<Forbidden> forbidden;
};


/**
 * The Authenticator interface allows us to implement different
 * authenticators based on the scheme (e.g. Basic, Digest, SPNEGO).
 */
class Authenticator
{
public:
  virtual ~Authenticator() {}

  /**
   * Authenticates the given HTTP request.
   *
   * NOTE: Libprocess does not perform any timeout handling of
   * the returned future, it is thus required that this future
   * is satisfied within a finite amount of time! Otherwise,
   * the socket will be remain open indefinitely!
   */
  // TODO(arojas): Add support for per-connection authentication.
  // Note that passing the socket is dangerous here because the
  // authenticator may hold a copy preventing the reference
  // counted socket from being closed.
  virtual Future<AuthenticationResult> authenticate(
      const Request& request) = 0;

  /**
   * Returns the name of the authentication scheme implemented.
   */
  virtual std::string scheme() const = 0;
};


/**
 * Implements the "Basic" authentication scheme using a
 * fixed set of credentials.
 */
class BasicAuthenticator : public Authenticator
{
public:
  BasicAuthenticator(
      const std::string& realm,
      const hashmap<std::string, std::string>& credentials);

  ~BasicAuthenticator() override;

  Future<AuthenticationResult> authenticate(
      const http::Request& request) override;

  std::string scheme() const override;

private:
  Owned<BasicAuthenticatorProcess> process_;
};


#ifdef USE_SSL_SOCKET

/**
 * Implements the "Bearer" authentication scheme using JSON Web Tokens.
 *
 * The authenticator uses a JWT implementation that is compliant with
 * RFC 7519, validating 'HS256' signed tokens using the specified
 * secret key. If the authentication was successful, the claims of the
 * returned principal will be set to the claims within the token's
 * payload.
 */
class JWTAuthenticator : public Authenticator
{
public:
  JWTAuthenticator(const std::string& realm, const std::string& secret);

  ~JWTAuthenticator() override;

  Future<AuthenticationResult> authenticate(
      const http::Request& request) override;

  std::string scheme() const override;

private:
  Owned<JWTAuthenticatorProcess> process_;
};
#endif // USE_SSL_SOCKET

} // namespace authentication {
} // namespace http {
} // namespace process {

#endif // __PROCESS_AUTHENTICATOR_HPP__
