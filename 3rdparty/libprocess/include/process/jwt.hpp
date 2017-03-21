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

#ifndef __PROCESS_JWT_HPP__
#define __PROCESS_JWT_HPP__

#include <ostream>
#include <string>

#include <stout/json.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

namespace process {
namespace http {
namespace authentication {

// Represents the various errors that can be returned when parsing or
// creating JSON Web Tokens. This can be useful to create proper
// responses to HTTP requests that included a token.
class JWTError : public Error {
public:
  enum class Type {
    INVALID_TOKEN, // Invalid token.
    UNKNOWN        // Internal error/all other errors.
  };

  JWTError(const std::string& message, Type _type)
    : Error(message), type(_type) {};

  const Type type;
};


/**
 * A JSON Web Token (JWT) implementation.
 * @see <a href="https://tools.ietf.org/html/rfc7519">RFC 7519</a>
 *
 * This implementation supports the 'none' and 'HS256' algorithms.
 * Header parameters other than 'alg' and 'typ' aren't parsed. To comply
 * with RFC 7515, headers with 'crit' parameter are invalid.
 * Currently, only the 'exp' standard claim is validated. Applications
 * that need to validate other claims need to do this in their
 * validation logic.
 */
class JWT
{
public:
  enum class Alg
  {
    None,
    HS256
  };

  struct Header
  {
    Alg alg;
    Option<std::string> typ;
  };

  /**
   * Parse an unsecured JWT.
   *
   * @param token The JWT to parse.
   *
   * @return The JWT representation if successful otherwise an Error.
   */
  static Try<JWT, JWTError> parse(const std::string& token);

  /**
   * Parse a JWT and validate its HS256 signature.
   *
   * @param token The JWT to parse.
   * @param secret The secret to validate the signature with.
   *
   * @return The validated JWT representation if successful otherwise an
   *     Error.
   */
  static Try<JWT, JWTError> parse(
      const std::string& token,
      const std::string& secret);

  /**
   * Create an unsecured JWT.
   *
   * @param payload The payload of the JWT.
   *
   * @return The unsecured JWT representation if successful otherwise an
   *     Error.
   */
  static Try<JWT, JWTError> create(const JSON::Object& payload);

  /**
   * Create a JWT with a HS256 signature.
   *
   * When creating a payload keep in mind that of the standard claims
   * currently only 'exp' is validated during parsing.
   *
   * @param payload The payload of the JWT
   * @param secret The secret to sign the JWT with.
   *
   * @return The signed JWT representation if successful otherwise an
   *     Error.
   */
  static Try<JWT, JWTError> create(
      const JSON::Object& payload,
      const std::string& secret);

  const Header header;
  const JSON::Object payload;
  const Option<std::string> signature;

private:
  JWT(const Header& header,
      const JSON::Object& payload,
      const Option<std::string>& signature);
};

std::ostream& operator<<(std::ostream& stream, const JWT& jwt);

} // namespace authentication {
} // namespace http {
} // namespace process {

#endif // __PROCESS_JWT_HPP__
